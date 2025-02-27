#include <typechecker.h>

#include <ast.h>
#include <error.h>
#include <parser.h>
#include <utils.h>
#include <vector.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIAG(diag, loc, ...) issue_diagnostic(diag, (ast)->filename.data, as_span((ast)->source), (loc), __VA_ARGS__)

#define ERR(loc, ...)                                                   \
  do {                                                                  \
    issue_diagnostic(DIAG_ERR, (ast)->filename.data, as_span((ast)->source), (loc), __VA_ARGS__); \
    return false;                                                       \
  } while (0)
#define SORRY(loc, ...)                         \
  do {                                          \
    DIAG(DIAG_SORRY, loc, __VA_ARGS__);         \
    return false;                               \
  } while (0)

#define ERR_DONT_RETURN(loc, ...) issue_diagnostic(DIAG_ERR, (ast)->filename.data, as_span((ast)->source), (loc), __VA_ARGS__)

#define ERR_NOT_CONVERTIBLE(where, to, from) ERR(where, "Type '%T' is not convertible to '%T'", from, to)

/// Check how well from is convertible to to.
///
/// \param to_type The type to convert to.
/// \param from_type The type to convert from.
/// \return -1 if the types are not convertible to one another.
/// \return 0 if the types are equivalent.
/// \return 1 if the types are convertible, but implicit conversions are required.
NODISCARD static isz convertible_score(Type *to_type, Type *from_type) {
  /// Expand types.
  Type *to_alias = type_last_alias(to_type);
  Type *from_alias = type_last_alias(from_type);

  /// Any type is implicitly convertible to void.
  if (type_is_void(to_alias)) return 0;

  /// If either type is NULL for some reason, we give up.
  if (!to_alias || !from_alias) return -1;

  /// If both are incomplete, compare the names.
  IncompleteResult res = compare_incomplete(to_alias, from_alias);
  if (res.incomplete) return res.equal ? 0 : -1;

  /// If the types are the same, they are convertible.
  Type *to = type_canonical(to_alias);
  Type *from = type_canonical(from_alias);
  if (type_equals_canon(to, from)) return 0;

  /// A function type is implicitly convertible to its
  /// corresponding pointer type.
  if (to->kind == TYPE_POINTER && from->kind == TYPE_FUNCTION) {
    Type *base = type_canonical(to->pointer.to);
    if (!type_is_incomplete_canon(base) && type_equals_canon(base, from)) return 0;
    return -1;
  }
  if (from->kind == TYPE_POINTER && to->kind == TYPE_FUNCTION) {
    Type *base = type_canonical(from->pointer.to);
    if (!type_is_incomplete_canon(base) && type_equals_canon(base, to)) return 0;
    return -1;
  }

  // A reference type is convertible to it's base type, and vis versa.
  if (from->kind == TYPE_REFERENCE && to->kind == TYPE_REFERENCE)
    return convertible_score(to->reference.to, from->reference.to);
  if (from->kind == TYPE_REFERENCE)
    return convertible_score(to, from->reference.to);
  if (to->kind == TYPE_REFERENCE)
    return convertible_score(to->reference.to, from);

  /// Smaller integer types are implicitly convertible to larger
  /// integer types if the type being converted to is signed, or
  /// if the smaller type is unsigned.
  bool to_is_int = type_is_integer_canon(to);
  bool from_is_int = type_is_integer_canon(from);

  if (to_is_int && from_is_int) {
    usz to_sz = type_sizeof(to);
    bool to_sign = type_is_signed_canon(to);
    usz from_sz = type_sizeof(from);
    bool from_sign = type_is_signed_canon(from);
    // Exactly equal integers.
    if (to_sz == from_sz && to_sign == from_sign) return 0;
    // Convertible integers.
    // TODO/FIXME: I have no idea if this is correct, it's just what was here before.
    if (to_sz > from_sz && (to_sign || !from_sign))
      return 1;
  }

  /// Integer literals are convertible to any integer type.
  if (from == t_integer_literal && to_is_int) return 1;

  // An array type is convertible to another array type if `from` size
  // is less than or equal to `to` size, as well as the element type
  // being convertible.
  if (from->kind == TYPE_ARRAY && to->kind == TYPE_ARRAY) {
    if (from->array.size > to->array.size)
      return -1;
    return convertible_score(to->array.of, from->array.of);
  }

  /// Otherwise, the types are not convertible.
  return -1;
}

/// Check if from is convertible to to.
/// FIXME: This should both check if the conversion is possible
///     and also perform if (unless it’s called during overload resolution).
///     Note that whenever we attempt to convert a reference to something,
///     we need to load the value. For that, we should insert something
///     like an ‘lvalue-to-rvalue cast expression’. This should also
///     let us eliminate all the type_is_reference checks we’re performing
///     when we generate IR for call expressions etc.
NODISCARD static bool convertible(Type *to_type, Type *from_type) {
  return convertible_score(to_type, from_type) != -1;
}

/// Get the common type of two types.
NODISCARD static Type *common_type(Type *a, Type *b) {
  Type *ta = type_canonical(a);
  Type *tb = type_canonical(b);
  if (type_equals(a, b)) return a;

  /// Some integer types are implicitly convertible to other integer types.
  /// See also `convertible_score`.
  if (type_is_integer(ta) && type_is_integer(tb)) {
    if (
        ta->primitive.size > tb->primitive.size
        && (ta->primitive.is_signed || !tb->primitive.is_signed)
    ) return ta;
    if (
        tb->primitive.size > ta->primitive.size
        && (tb->primitive.is_signed || !ta->primitive.is_signed)
    ) return tb;
  }

  /// No common type.
  return NULL;
}

/// An overload candidate.
typedef struct Candidate {
  Symbol *symbol;
  size_t score;

  /// Whether the overload is valid or why it is invalid.
  enum {
    candidate_valid,
    invalid_parameter_count,        /// Candidate has too many/few parameters.
    invalid_argument_type,          /// Argument type is not convertible to parameter type.
    invalid_too_many_conversions,   /// Candidate is valid but not ideal.
    invalid_expected_type_mismatch, /// Candidate is not equivalent to the expected type of the parent expression.
    invalid_no_dependent_callee,    /// Candidate is an argument of a call with no matching callee.
    invalid_no_dependent_arg,       /// No matching overload for argument of function type.
  } validity;

  /// Index of the incompatible argument.
  usz invalid_arg_index;
} Candidate;

/// A set of overload candidates.
typedef Vector(Candidate) OverloadSet;

/// Collect all possible overload candidates for a function reference.
static OverloadSet collect_overload_set(Node *func) {
  OverloadSet overload_set = {0};
  for (Scope *scope = func->funcref.scope; scope; scope = scope->parent) {
    foreach_val (sym, scope->symbols) {
      if (sym->kind != SYM_FUNCTION) {
        continue;
      }
      if (string_eq(sym->name, func->funcref.name)) {
        Candidate s = {0};
        s.symbol = sym;
        s.score = 0;
        s.validity = candidate_valid;
        vector_push(overload_set, s);
      }
    }
  }
  return overload_set;
}

/// Actually resolve a function.
///
/// The overloads sets passed to this function must be minimal, i.e.
/// all overloads that are not viable must already be marked as such.
///
/// \param ast The AST of the program.
/// \param overload_set The overload set of the symbol being resolved.
/// \param funcref The symbol being resolved.
/// \param dependent_overload_set (optional) The overload set of the callee if this is a function argument.
/// \param dependent_funcref (optional) The callee if this is a function argument.
/// \return Whether overload resolution was successful.
NODISCARD static bool resolve_overload(
  Module *ast,
  OverloadSet *overload_set,
  Node *funcref,
  OverloadSet *dependent_overload_set,
  Node *dependent_funcref
) {
  /// Determine the overloads that are still valid.
  Symbol *valid_overload = NULL;
  bool ambiguous = false;
  foreach (sym, *overload_set) {
    if (sym->validity != candidate_valid) continue;

    /// If O(F) contains more than one element, then the program is
    /// ill-formed: F is ambiguous.
    if (valid_overload) {
      ERR_DONT_RETURN(funcref->source_location, "Use of overloaded function is ambiguous.");

      /// Print the valid overloads.
      ambiguous = true;
      break;
    }

    /// Otherwise, save this overload for later.
    valid_overload = sym->symbol;
  }

  /// If O(F) is empty, then the program is ill-formed: there is no
  /// matching overload for F.
  if (!valid_overload || ambiguous) {
    if (!(funcref->parent->kind == NODE_CALL) && !ambiguous) {
      ERR(funcref->source_location, "Unknown Symbol");
    }

    /// Print parameter types if this is a call and there is at least one argument.
    if (funcref->parent->kind == NODE_CALL && funcref->parent->call.arguments.size) {
      eprint("\n    %B38Where%m\n");
      size_t index = 1;
      foreach_val (arg, funcref->parent->call.arguments)
        eprint("    %Z = %T\n", index++, arg->type);
    }

    /// Print all overloads.
    size_t index = 1;
    if (!ambiguous) eprint("\n    %B38Overload Set%m\n");
    else eprint("\n    %B38Candidates%m\n");
    foreach (c, *overload_set) {
      if (ambiguous && c->validity != candidate_valid) continue;
      u32 line;
      seek_location(as_span(ast->source), c->symbol->val.node->source_location, &line, NULL, NULL);
      eprint("    %B38(%Z) %32%S %31: %T %m(%S:%u)\n",
        index++, c->symbol->name, c->symbol->val.node->type, ast->filename, line);
    }

    /// If the call is ambiguous, then we’re done.
    if (ambiguous) return false;

    /// We might want to print dependent overload sets.
    Vector(Node*) dependent_functions = {0};
    Vector(span) dependent_function_names = {0};

    /// Explain why each one is invalid.
    eprint("\n    %B38Invalid Overloads%m\n");
    index = 1;
    foreach (c, *overload_set) {
      eprint("    %B38(%Z) %m", index++);
      switch (c->validity) {
        default: ICE("Unknown overload invalidation reason: %d", c->validity);

        /// We only get here if there are *no* valid candidates.
        case candidate_valid: ICE("candidate_valid not allowed here");

        /// Candidates are only invalidated with this error if there
        /// is at least one candidate that is otherwise valid, which,
        /// as we’ve just established, is impossible.
        case invalid_too_many_conversions: ICE("too_many_conversions not allowed here");

        /// Not enough / too many parameters.
        case invalid_parameter_count:
          eprint("Candidate takes %Z parameters, but %Z were provided",
                 c->symbol->val.node->type->function.parameters.size,
                 funcref->parent->call.arguments.size);
          break;

        /// Argument type is not convertible to parameter type.
        case invalid_argument_type:
          eprint("Argument of type '%T' is not convertible to parameter type '%T'.",
                 funcref->parent->call.arguments.data[c->invalid_arg_index]->type,
                 c->symbol->val.node->type->function.parameters.data[c->invalid_arg_index].type);
          break;

        /// TODO: Print a better error depending on the parent expression.
        case invalid_expected_type_mismatch:
          eprint("Candidate type '%T' is not convertible to '%T'",
                 c->symbol->val.node->type,
                 funcref->parent->type);
          break;

        /// No matching overload for argument of function type. Only
        /// arguments can be set to this validity.
        case invalid_no_dependent_callee:
          ASSERT(dependent_funcref);
          ASSERT(dependent_overload_set);
          eprint("Candidate type '%T' is not convertible to parameter type '%T'",
                 c->symbol->val.node->type,
                 dependent_funcref->parent->call.arguments.data[c->invalid_arg_index]->type);
          break;

        /// No matching overload for callee. Only callees can be set to this
        /// validity.
        case invalid_no_dependent_arg: {
          Node * arg = funcref->parent->call.arguments.data[c->invalid_arg_index];
          Node * param = c->symbol->val.node->function.param_decls.data[c->invalid_arg_index];
          eprint("No overload of %32%S%m with type %T", arg->funcref.name, param->type);

          /// Mark that we need to print the overload set of this function too.
          span *ptr = vector_find_if(n, dependent_function_names, string_eq(*n, arg->funcref.name));
          if (!ptr) {
            vector_push(dependent_functions, arg);
            vector_push(dependent_function_names, as_span(arg->funcref.name));
          }
        } break;
      }
      eprint("\n");
    }

    /// Print the overload sets of all dependent functions.
    if (dependent_functions.size) {
      eprint("\n    %B38Dependent Overload Sets%m\n");
      foreach_val (n, dependent_functions) {
        eprint("        %B38Overloads of %B32%S%B38%m\n", n->funcref.name);

        OverloadSet o = collect_overload_set(n);
        foreach (c, o) {
          u32 line;
          seek_location(as_span(ast->source), c->symbol->val.node->source_location, &line, NULL, NULL);
          eprint("        %32%S %31: %T %m(%S:%u)\n",
            c->symbol->name, c->symbol->val.node->type, ast->filename, line);
        }
        vector_delete(o);
      }
    }

    return false;
  }

  /// Otherwise, resolve F to the last remaining element of O(F).
  funcref->funcref.resolved = valid_overload;
  funcref->type = funcref->funcref.resolved->val.node->type;
  return true;
}

/// Remove overloads except those with the least implicit conversions.
void reduce_overload_set(OverloadSet *overload_set) {
  if (overload_set->size) {
    /// Determine the candidate with the least number of implicit conversions.
    usz min_score = overload_set->data[0].score;
    foreach (sym, *overload_set)
      if (sym->validity == candidate_valid && sym->score < min_score)
        min_score = sym->score;

    /// Remove all candidates with a more implicit conversions.
    foreach (sym, *overload_set)
      if (sym->validity == candidate_valid && sym->score > min_score)
        sym->validity = invalid_too_many_conversions;
  }
}

/// Resolve a function reference.
///
/// Terminology:
///
///   - A (formal) parameter is a parameter (type) of a function type or signature.
///
///   - An (actual) argument is a subexpression of a function call that is not
///     the callee.
///
///   - Two types, A and B, are *equivalent* iff
///       - 1. A and B are the same type, or
///       - 2. one is a function type and the other its corresponding function
///            pointer type, or
///       - 3. one is a named type whose underlying type is equivalent to the
///            other.
///
///   - A type A is *convertible* to a type B if there is a series of implicit
///     conversions that transforms A to B or if A and B are equivalent.
///
///   - An argument A is convertible/equivalent to a parameter P iff the type
///     of A is convertible/equivalent to the type of P.
///
/// To resolve an unresolved function reference, execute the following steps in
/// order. The unresolved function reference in question is hereinafter referred
/// to as ‘the function being resolved’.
NODISCARD static bool resolve_function(Module *ast, Node *func) {
  /// 0. Skip anything that is not a function reference, or any function
  ///    references previously resolved.
  if (func->kind != NODE_FUNCTION_REFERENCE || func->funcref.resolved)
    return true;

  /// 1. Collect all functions with the same name as the function being
  ///    resolved into an *overload set* O. We cannot filter out any
  ///    functions just yet.
  OverloadSet overload_set = collect_overload_set(func);
  OverloadSet arg_overload_set = {0};

  /// Better error message in case we have an empty overload set.
  if (overload_set.size == 0) ERR(func->source_location, "Unknown symbol");

  // Extra validation step: ensure all functions within overload set
  // have matching return type.
  Type *return_type = NULL;
  foreach (candidate, overload_set) {
    if (!return_type) {
      return_type = candidate->symbol->val.node->type->function.return_type;
      continue;
    }
    if (!type_equals(candidate->symbol->val.node->type->function.return_type, return_type))
      ERR(candidate->symbol->val.node->source_location,
          "Function in overload set has mismatched return type %T (expecting %T)",
          candidate->symbol->val.node->type->function.return_type, return_type);
  }

  /// Whether there was an error.
  bool ok = true;

 step2:
  /// 2. If the parent expression is a call expression, and the function being
  ///    resolved is the callee of the call, then:
  if (!func->parent) {
    print("No parent of function; can not resolve function properly\n"
          "  \"%S\"\n",
          func->funcref.name);
    goto err;
  }

  if (func->parent->kind == NODE_CALL && func == func->parent->call.callee) {
    Node *call = func->parent;

    /// 2a. Typecheck all arguments of the call that are not unresolved
    ///     function references themselves. Note: This takes care of
    ///     resolving nested calls.
    foreach_val (arg, call->call.arguments)
      if (arg->kind != NODE_FUNCTION_REFERENCE)
        if (!typecheck_expression(ast, arg))
          goto err;

    /// 2b. Remove from O all functions that have a different number of
    ///     parameters than the call expression has arguments.
    foreach (sym, overload_set)
      if (sym->symbol->val.node->type->function.parameters.size != call->call.arguments.size)
        sym->validity = invalid_parameter_count;


    /// 2c. Let A_1, ... A_n be the arguments of the call expression.
    ///
    /// 2d. For candidate C in O, let P_1, ... P_n be the parameters of C.
    ///     For each argument A_i of the call, iff it is not an unresolved
    ///     function, check if it is convertible to P_i. Remove C from O if
    ///     it is not. Note down the number of A_i’s that required a (series
    ///     of) implicit conversions to their corresponding P_i’s.
    ///
    ///     Also collect unresolved function references.
    /// TODO: Could optimise this by merging with above loop.
    typedef struct { usz index; OverloadSet overloads; } unresolved_func;
    Vector(unresolved_func) unresolved_functions = {0};
    foreach (candidate, overload_set) {
      if (candidate->validity != candidate_valid) continue;
      foreach_index (i, call->call.arguments) {
        /// Note down the number of function references.
        Node *arg = call->call.arguments.data[i];
        if (arg->kind == NODE_FUNCTION_REFERENCE && ! arg->funcref.resolved) {
          unresolved_func uf = {0};
          uf.index = i;
          vector_push(unresolved_functions, uf);
          continue;
        }

        /// Check if the argument is convertible to the parameter.
        Type *param_type = candidate->symbol->val.node->type->function.parameters.data[i].type;
        isz score = convertible_score(param_type, arg->type);
        if (score == -1) {
          candidate->validity = invalid_argument_type;
          candidate->invalid_arg_index = i;
          break;
        }

        /// If it is, check if a conversion was required.
        candidate->score += (usz) score;
      }
    }

    /// 2e. If there are unresolved function references.
    if (unresolved_functions.size) {
      /// 2eα. Collect their overload sets.
      foreach (uf, unresolved_functions) {
        uf->overloads = collect_overload_set(call->call.arguments.data[uf->index]);

        /// Confidence check.
        if (!uf->overloads.size) {
          ERR_DONT_RETURN(call->call.arguments.data[uf->index]->source_location, "Unknown symbol");
          goto cleanup_arg_overloads;
        }

        /// 2eβ. Remove from O all candidates C that do no accept any overload
        ///      of this argument as a parameter.
        foreach (candidate, overload_set) {
          if (candidate->validity != candidate_valid) continue;
          Type *param_type = candidate->symbol->val.node->type->function.parameters.data[uf->index].type;

          bool found = false;
          foreach (arg_candidate, uf->overloads) {
            if (convertible_score(param_type, arg_candidate->symbol->val.node->type) == 0) {
              found = true;
              break;
            }
          }

          if (!found) {
            candidate->validity = invalid_no_dependent_arg;
            candidate->invalid_arg_index = uf->index;
          }
        }
      }

      /// 2eγ. Remove from O all functions except those with the least number of
      ///     implicit conversions as per step 2d.
      reduce_overload_set(&overload_set);

      /// 2eδ. Resolve the function being resolved.
      if (!resolve_overload(ast, &overload_set, func, NULL, NULL)) goto cleanup_arg_overloads;

      /// 2eε. For each argument, remove from its overload set all candidates
      /// that are not equivalent to the type of the corresponding parameter
      /// of the resolved function.
      foreach (uf, unresolved_functions) {
        foreach (candidate, uf->overloads) {
          if (candidate->validity != candidate_valid) continue;
          Type *param_type = func->type->function.parameters.data[uf->index].type;
          if (convertible_score(param_type, candidate->symbol->val.node->type) != 0) {
            candidate->validity = invalid_no_dependent_callee;
            candidate->invalid_arg_index = uf->index;
          }
        }

        /// 2eζ. Resolve the argument.
        if (!resolve_overload(ast, &uf->overloads, call->call.arguments.data[uf->index], &overload_set, func))
          goto cleanup_arg_overloads;
      }

      /// Success, yay!
      goto done;

      /// Cleanup so we don’t leak memory.
    cleanup_arg_overloads:
      foreach (uf, unresolved_functions)
        vector_delete(uf->overloads);
      vector_delete(unresolved_functions);
      goto err;
    }

    /// 2f. Remove from O all functions except those with the least number of
    ///     implicit conversions as per step 2d.
    ///
    /// Note: If we get here, then unresolved_functions is empty, so no cleanup
    /// required.
    reduce_overload_set(&overload_set);
  }

  /// 3. Otherwise, depending on the type of the parent expression,
  else {
    Node *parent = func->parent;
    switch (parent->kind) {
    /// 3a. If the parent expression is a unary prefix expression with operator
    ///     address-of, then replace the parent expression with the unresolved
    ///     function and go to step 2/3 depending on the type of the new parent.
    case NODE_UNARY: {
      if (parent->unary.op == TK_AMPERSAND) {
        Node *grandparent = parent->parent;
        ast_replace_node(ast, parent, func);
        func->parent = grandparent;
        goto step2;
      }
    } break;

    /// 3b. If the parent expression is a declaration,
    case NODE_DECLARATION: {
      Type *decl_type = parent->type;
      /// ... and the lvalue is not of function pointer type, this is a type error.
      if (decl_type->kind != TYPE_POINTER || decl_type->pointer.to->kind != TYPE_FUNCTION) {
        ERR_DONT_RETURN(func->source_location,
          "Overloaded function %S is not convertible to %T\n",
          func->funcref.name, decl_type);
        goto err;
      }

      /// Otherwise, remove from O all functions that are not equivalent to the
      /// lvalue being assigned to.
      foreach (sym, overload_set)
        if (sym->validity == candidate_valid && convertible_score(decl_type, sym->symbol->val.node->type) != 0)
          sym->validity = invalid_expected_type_mismatch;
    } break;

    /// 3c. If the parent expression is an assignment expression, then
    case NODE_BINARY: {
      if (parent->binary.op != TK_COLON_EQ) break;

      /// ... if we are the LHS, then this is a type error, as we cannot assign to a
      /// function reference.
      if (func == parent->binary.lhs) {
        if (overload_set.size) ERR(func->source_location, "Cannot assign to function '%S'", func->funcref.name);
        else ERR(func->source_location, "Unknown symbol '%S'", func->funcref.name);
      }
      ASSERT(func == parent->binary.rhs);

      /// If the lvalue is not of function pointer type, this is a type error.
      Type *lvalue_type = parent->binary.lhs->type;
      if (lvalue_type->kind != TYPE_POINTER || lvalue_type->pointer.to->kind != TYPE_FUNCTION) {
        ERR_DONT_RETURN(func->source_location,
          "Overloaded function %S is not convertible to %T\n",
          func->funcref.name, lvalue_type);
        goto err;
      }

      /// Otherwise, remove from O all functions that are not equivalent to
      /// the lvalue being assigned to.
      foreach (sym, overload_set)
        if (sym->validity == candidate_valid && convertible_score(lvalue_type, sym->symbol->val.node->type) != 0)
          sym->validity = invalid_expected_type_mismatch;
    } break;

    /// TODO: Infer return value of block expressions.
    /// 3d. If the parent expression is a return expression, and the return type of the
    ///     function F containing that return expression is not of function pointer type,
    ///     this is a type error. Otherwise, remove from O all functions that are not
    ///     equivalent to the return type of F.
    /*
    // TODO: unimplemented
    case NODE_RETURN: {
      // FIXME: Should refer to return type of function that `parent` is within.
      Type *return_type = parent->type;
      if (return_type->kind == TYPE_POINTER && return_type->pointer.to->kind == TYPE_FUNCTION) {
        foreach_if (OverloadedFunctionSymbol, sym, overload_set, sym->validity == candidate_valid)
          if (convertible_score(ast, return_type, sym->symbol->node->type) != 0)
            sym->validity = invalid_expected_type_mismatch;
        break;
      }

      ERR_DONT_RETURN(func->source_location,
        "Overloaded function %S is not convertible to %T\n",
          func->funcref.name return_type);
      goto err;
    } break;
    */

    /// 3e. If the parent expression is a cast expression, then, ...
    case NODE_CAST: {
      Type *cast_type = parent->type;

      /// ... if the result type of the cast is a function or function pointer type,
      /// remove from O all functions that are not equivalent to that type.
      if ((cast_type->kind == TYPE_POINTER && cast_type->pointer.to->kind == TYPE_FUNCTION) ||
          (cast_type->kind == TYPE_FUNCTION)) {
        foreach (sym, overload_set)
          if (sym->validity == candidate_valid && convertible_score(cast_type, sym->symbol->val.node->type) != 0)
            sym->validity = invalid_expected_type_mismatch;
      }
    } break;

    /// 3f. Otherwise, do nothing.
    default: break;
    }
  }

  /// 4. Resolve the function reference.
  ok = resolve_overload(ast, &overload_set, func, NULL, NULL);

  /// Clean up the vectors.
 done:
  vector_delete(overload_set);
  vector_delete(arg_overload_set);

  /// Done.
  return ok;

  /// There was an error. Free everything and return false.
 err:
  ok = false;
  goto done;
}

NODISCARD static bool typecheck_type(Module *ast, Type *t) {
  if (t->type_checked) return true;
  t->type_checked = true;
  switch (t->kind) {
  default: ICE("Invalid type kind of type %T", t);
  case TYPE_PRIMITIVE: return true;
  case TYPE_POINTER: return typecheck_type(ast, t->pointer.to);
  case TYPE_REFERENCE: return typecheck_type(ast, t->reference.to);

  case TYPE_NAMED: {
    if (t->named->val.type)
      return typecheck_type(ast, t->named->val.type);
    return true;
  }

  case TYPE_FUNCTION:
    if (!typecheck_type(ast, t->function.return_type)) return false;
    foreach (param, t->function.parameters) {
      if (!typecheck_type(ast, param->type)) return false;
      if (type_is_incomplete(param->type))
        ERR(param->source_location, "Function parameter must not be of incomplete type");
    }
    return true;

  case TYPE_ARRAY:
    if (!typecheck_type(ast, t->array.of)) return false;
    if (!t->array.size)
      ERR(t->source_location,
          "Cannot create array of zero size: %T", t);
    return true;

  case TYPE_STRUCT:
    foreach (member, t->structure.members) {
      if (!typecheck_type(ast, member->type)) return false;
    }

    // If a struct already has it's alignment set, then we will keep the
    // alignment of the struct to what it was set to, assuming that whoever
    // did it knows what they are doing.
    bool has_alignment = t->structure.alignment;
    foreach (member, t->structure.members) {
      size_t alignment = type_alignof(member->type);
      if (!has_alignment && alignment > t->structure.alignment)
        t->structure.alignment = alignment;

      t->structure.byte_size = ALIGN_TO(t->structure.byte_size, alignment);

      member->byte_offset = t->structure.byte_size;
      t->structure.byte_size += type_sizeof(member->type);
    }

    if (t->structure.alignment)
      t->structure.byte_size = ALIGN_TO(t->structure.byte_size, t->structure.alignment);

    return true;

  case TYPE_INTEGER: {
    if (!t->integer.bit_width)
      ERR(t->source_location, "Rejecting arbitrary integer of zero width: %T", t);

    // TODO: This should probably be backend-dependant.
    if (t->integer.bit_width > 64)
      SORRY(t->source_location, "Rejecting arbitrary integer of width greater than 64: %T. This is a WIP, sorry!", t);

    return true;
  }
  }
  UNREACHABLE();
}

/// Check if a call is an intrinsic.
///
/// \param callee The callee to check.
/// \return The intrinsic number if it is an intrinsic, or I_BUILTIN_COUNT otherwise.
NODISCARD static enum IntrinsicKind intrinsic_kind(Node *callee) {
    STATIC_ASSERT(INTRIN_COUNT == 7, "Handle all intrinsics in sema");
    if (callee->kind != NODE_FUNCTION_REFERENCE) return INTRIN_COUNT;
    if (string_eq(callee->funcref.name, literal_span("__builtin_syscall"))) return INTRIN_BUILTIN_SYSCALL;
    if (string_eq(callee->funcref.name, literal_span("__builtin_inline"))) return INTRIN_BUILTIN_INLINE;
    if (string_eq(callee->funcref.name, literal_span("__builtin_line"))) return INTRIN_BUILTIN_LINE;
    if (string_eq(callee->funcref.name, literal_span("__builtin_filename"))) return INTRIN_BUILTIN_FILENAME;
    if (string_eq(callee->funcref.name, literal_span("__builtin_debugtrap"))) return INTRIN_BUILTIN_DEBUGTRAP;
    if (string_eq(callee->funcref.name, literal_span("__builtin_memcpy"))) return INTRIN_BUILTIN_MEMCPY;
    return INTRIN_COUNT;
}

/// This is how we handle intrinsics:
///
/// There is a `NODE_INTRINSIC_CALL` AST node that is only generated here; it
/// is just like a call expression, but the ‘callee’ is an intrinsic and stored
/// as an id.
///
/// That node is lowered during IR generation to either IR instructions or
/// an `IR_INTRINSIC` instruction. The operands are the ‘call arguments’ and
/// are stored just like the arguments to a call instruction; the intrinsic
/// id is stored in a separate member.
///
/// Any `IR_INTRINSIC` instructions are lowered either to other MIR instructions
/// or to a `MIR_INTRINSIC` instruction whose first operand is the intrinsic id
/// (e.g. `INTRIN_BUILTIN_SYSCALL`) and whose other operands are the operands
/// of the intrinsic.
///
/// Any `MIR_INTRINSIC` instruction are lowered either via the ISel table or
/// manually in the backend.
NODISCARD static bool typecheck_intrinsic(Module *ast, Node *expr) {
    ASSERT(expr->kind == NODE_CALL);
    ASSERT(expr->call.callee->kind == NODE_FUNCTION_REFERENCE);

    STATIC_ASSERT(INTRIN_COUNT == 7, "Handle all intrinsics in sema");
    switch (expr->call.intrinsic) {
        case INTRIN_COUNT:
        case INTRIN_BACKEND_COUNT:
          UNREACHABLE();

        /// This has 1-7 integer-sized arguments and returns an integer.
        case INTRIN_BUILTIN_SYSCALL: {
            if (expr->call.arguments.size < 1 || expr->call.arguments.size > 7)
                ERR(expr->source_location, "__builtin_syscall() intrinsic takes 1 to 7 arguments");

            foreach_index (i, expr->call.arguments) {
                Node* arg = expr->call.arguments.data[i];
                if (!typecheck_expression(ast, arg)) return false;
                if (type_is_incomplete(arg->type))
                    ERR(arg->source_location, "Argument of __builtin_syscall() may not be incomplete");

                /// Make sure the argument fits in a register.
                usz sz = type_sizeof(arg->type);
                if (sz > type_sizeof(t_integer))
                    ERR(arg->source_location, "Argument of __builtin_syscall() must be integer-sized or smaller");

                /// Extend to register size if need be.
                if (sz != type_sizeof(t_integer)) {
                    Node* cast = ast_make_cast(ast, arg->source_location, t_integer, arg);
                    if (!typecheck_expression(ast, cast)) return false;
                    arg->parent = cast;
                    expr->call.arguments.data[i] = cast;
                }
            }

            /// Return type is integer.
            expr->kind = NODE_INTRINSIC_CALL;
            expr->type = t_integer;
            return true;
        }

        /// This takes one argument, and it must be a call expression.
        case INTRIN_BUILTIN_INLINE: {
          if (expr->call.arguments.size != 1)
            ERR(expr->source_location, "__builtin_inline() requires exactly one argument");
          Node* call = expr->call.arguments.data[0];
          if (!typecheck_expression(ast, call)) return false;
          if (call->kind != NODE_CALL)
            ERR(expr->source_location, "Argument of __builtin_inline() must be a call expression");

          /// Return type is the return type of the call.
          expr->kind = NODE_INTRINSIC_CALL;
          expr->type = call->type;
          return true;
        }

        /// This takes no arguments and returns an integer.
        case INTRIN_BUILTIN_LINE: {
          if (expr->call.arguments.size != 0)
            ERR(expr->source_location, "__builtin_line() takes no arguments");

          u32 line = 0;
          seek_location(as_span(ast->source), expr->source_location, &line, NULL, NULL);

          expr->type = t_integer_literal;
          expr->kind = NODE_LITERAL;
          expr->literal.type = TK_NUMBER;
          expr->literal.integer = line;
          return true;
        }

        /// This takes no arguments and returns a string.
        case INTRIN_BUILTIN_FILENAME: {
          if (expr->call.arguments.size != 0)
            ERR(expr->source_location, "__builtin_filename() takes no arguments");

          /// Remove everything up to the first path separator from the filename.
          const char *end = ast->filename.data + ast->filename.size;
          while (end > ast->filename.data && end[-1] != '/'
#ifdef _WIN32
                 && end[-1] != '\\'
#endif
          ) {
            end--;
          }

          expr->kind = NODE_LITERAL;
          expr->literal.type = TK_STRING;
          expr->literal.string_index = ast_intern_string(
            ast,
            (span){
              .data = end,
              .size = (usz) (ast->filename.data + ast->filename.size - end),
            }
          );

          string s = ast->strings.data[expr->literal.string_index];
          expr->type = ast_make_type_array(ast, expr->source_location, t_byte, s.size + 1);
          return true;
        }

        /// This is basically a breakpoint.
        case INTRIN_BUILTIN_DEBUGTRAP: {
          if (expr->call.arguments.size != 0)
            ERR(expr->source_location, "__builtin_debugtrap() takes no arguments");

          expr->kind = NODE_INTRINSIC_CALL;
          expr->type = t_void;
          return true;
        }

        /// Like C’s `memcpy()` function.
        case INTRIN_BUILTIN_MEMCPY: {
          if (expr->call.arguments.size != 3)
            ERR(expr->source_location, "__builtin_memcpy() takes exactly three arguments");

          if (!typecheck_expression(ast, expr->call.arguments.data[0])) return false;
          if (!typecheck_expression(ast, expr->call.arguments.data[1])) return false;
          if (!typecheck_expression(ast, expr->call.arguments.data[2])) return false;

          if (expr->call.arguments.data[0]->type->kind != TYPE_POINTER)
            ERR(expr->call.arguments.data[0]->source_location, "First argument of __builtin_memcpy() must be a pointer");
          if (expr->call.arguments.data[1]->type->kind != TYPE_POINTER)
            ERR(expr->call.arguments.data[1]->source_location, "Second argument of __builtin_memcpy() must be a pointer");
          if (!convertible(t_integer, expr->call.arguments.data[2]->type))
            ERR(expr->call.arguments.data[2]->source_location, "Third argument of __builtin_memcpy() must be an integer");

          expr->kind = NODE_INTRINSIC_CALL;
          expr->type = t_void;
          return true;
        }
    }

    UNREACHABLE();
}

NODISCARD bool typecheck_expression(Module *ast, Node *expr) {
  /// Don’t typecheck the same expression twice.
  if (expr->type_checked) return true;
  expr->type_checked = true;

  if (expr->type && !typecheck_type(ast, expr->type)) return false;

  /// Typecheck the expression.
  switch (expr->kind) {
    default: ICE("Invalid node type");

    /// Typecheck each child of the root.
    case NODE_ROOT:
    foreach_val (node, expr->root.children) {
        if (!typecheck_expression(ast, node))
          return false;

        if (node != vector_back(expr->root.children)) {
          if (node->kind == NODE_BINARY && node->binary.op == TK_EQ)
            ERR(node->source_location,
                "Comparison at top level; result unused. Did you mean to assign using %s?",
                token_type_to_string(TK_COLON_EQ));

          // If the function being called doesn't return void, it is being discarded.
          // TODO: We should ensure the function does *not* have a discardable
          // attribute. We will need to find the actual function node and not
          // just the function type; this means following funcrefs.
          ///
          /// This is currently only supported for direct calls.
          if (
            node->kind == NODE_CALL &&
            node->call.callee->kind == NODE_FUNCTION &&
            node->call.callee->type->function.return_type != t_void &&
            !node->call.callee->type->function.attr_discardable
          ) {
            ERR(
              node->source_location,
              "Discarding return value of function `%S` that was not declared `discardable`.",
              node->call.callee->function.name
            );
          }
        }
      }

      /// Replace function references in the root with the function nodes
      /// iff the source location of the function is the same as that of
      /// the function reference.
      ///
      /// This is so that if someone, for whatever reason, puts the name
      /// of the function as an expression in the root, it will just be
      /// removed rather than replaced with the function.
      foreach_index(i, expr->root.children) {
        Node *node = expr->root.children.data[i];
        if (node->kind == NODE_FUNCTION_REFERENCE) {
          Node *func = node->funcref.resolved->val.node;
          if (
            func &&
            func->source_location.start == node->source_location.start &&
            func->source_location.end == node->source_location.end
          ) { expr->root.children.data[i] = func; }
        }
      }

      /// If the last expression in the root is not of type integer,
      /// add a literal 0 so that `main()` returns 0. If the last
      /// expression is an integer, make sure to convert it to the
      /// right integer type.
      /// FIXME: Should be int, but that currently breaks the x86_64 backend.
      if (expr->root.children.size && convertible(t_integer, vector_back(expr->root.children)->type)) {
          Node* back = vector_back(expr->root.children);
          if (!type_equals(t_integer, back->type)) {
            Node *cast = ast_make_cast(
              ast,
              back->source_location,
              t_integer,
              back
            );
            ASSERT(typecheck_expression(ast, cast));
            back->parent = cast;
            vector_back(expr->root.children) = cast;
          }
      } else {
        Node *lit = ast_make_integer_literal(ast, (loc){0}, 0);
        vector_push(expr->root.children, lit);
        lit->parent = expr;
        ASSERT(typecheck_expression(ast, lit));
      }

      break;

    case NODE_MODULE_REFERENCE: break;

    /// Typecheck the function body if there is one.
    case NODE_FUNCTION: {
      if (!expr->function.body) break;
      if (!typecheck_expression(ast, expr->function.body)) return false;

      /// Make sure the return type of the body is convertible to that of the function.
      Type *ret = expr->type->function.return_type;
      Type *body = expr->function.body->type;
      if (!convertible(ret, body)) {
        loc l = {0};
        if (expr->function.body->kind == NODE_BLOCK)
          l = vector_back_or(expr->function.body->block.children, expr)->source_location;
        else l = expr->function.body->source_location;
        ERR(l,
            "Type '%T' of function body is not convertible to return type '%T'.",
            body, ret);
      }

      /// Validate attributes.
      TypeFunction *ftype = &expr->type->function;

      /// Noreturn functions always have side effects.
      if (ftype->attr_noreturn) {
        if (ftype->attr_const) ERR(expr->source_location, "Noreturn function cannot be const");
        if (ftype->attr_pure) ERR(expr->source_location, "Noreturn function cannot be pure");
      }

      /// This is obviously nonsense.
      if (ftype->attr_inline && ftype->attr_noinline)
        ERR(expr->source_location, "Function cannot be both inline and noinline");

      /// Make sure `used` doesn’t override any other linkage type.
      if (ftype->attr_used) {
        if (expr->function.linkage != LINKAGE_INTERNAL)
          ERR(expr->source_location, "Attribute `used` is not valid for this function");
        expr->function.linkage = LINKAGE_USED;
      }

      /// Warn about functions returning void annotated as discardable.
      if (ftype->attr_discardable && type_is_void(ftype->return_type))
        DIAG(DIAG_WARN, expr->source_location, "`discardable` has no effect on functions returning void");

    } break;

    /// Typecheck declarations.
    case NODE_DECLARATION: {
      /// If there is an initialiser, then its type must match the type of the variable.
      if (expr->declaration.init) {
        if (!typecheck_expression(ast, expr->declaration.init)) return false;
        // Type inference :^)
        if (!expr->type) {
          expr->type = expr->declaration.init->type;
          if (expr->type == t_integer_literal) expr->type = t_integer;
        } else if (!convertible(expr->type, expr->declaration.init->type))
          ERR_NOT_CONVERTIBLE(expr->declaration.init->source_location, expr->type, expr->declaration.init->type);

        if (expr->declaration.init->type == t_integer_literal)
          expr->declaration.init->type = expr->type;
        else if (expr->declaration.init->type->kind == TYPE_ARRAY &&
                 expr->declaration.init->type->array.of == t_integer_literal) {
          expr->declaration.init->type->array.of = expr->type->array.of;
          foreach_val (node, expr->declaration.init->literal.compound) {
            node->type = expr->type->array.of;
          }
        }

      } else if (!expr->type) ERR(expr->source_location, "Cannot infer type of declaration without initialiser");

      if (!typecheck_type(ast, expr->type)) return false;

      /// Strip arrays and recursive typedefs.
      Type *base_type = type_canonical(expr->type);
      Type *array = NULL;
      while (base_type) {
        if (base_type->kind == TYPE_NAMED) base_type = type_canonical(base_type->named->val.type);
        else if (base_type->kind == TYPE_ARRAY) {
          array = base_type;
          base_type = type_canonical(base_type->array.of);
          break;
        } else break;
      }

      /// Make sure this isn’t an array of incomplete type.
      if (type_is_incomplete(base_type)) {
        ERR(expr->source_location, "Cannot declare %s of incomplete type '%T'",
            array ? "array" : "variable", expr->type);
      }

      if (base_type->kind == TYPE_FUNCTION) {
        ERR(expr->source_location, "Cannot declare %s of function type '%T'",
            array ? "array" : "variable", expr->type);
      }
      } break;

    /// If expression.
    case NODE_IF:
      if (!typecheck_expression(ast, expr->if_.condition)) return false;
      if (!typecheck_expression(ast, expr->if_.then)) return false;

      /// If the then and else branch of an if expression both exist and have
      /// the a common type, then the type of the if expression is that type.
      if (expr->if_.else_) {
        if (!typecheck_expression(ast, expr->if_.else_)) return false;
        Type *common = common_type(expr->if_.then->type, expr->if_.else_->type);
        if (common) expr->type = common;
        else expr->type = t_void;
      }

      /// Otherwise, the type of the if expression is void.
      else { expr->type = t_void; }
      break;

    /// A while expression has type void.
    case NODE_WHILE:
      if (!typecheck_expression(ast, expr->while_.condition)) return false;
      if (!typecheck_expression(ast, expr->while_.body)) return false;
      expr->type = t_void;
      break;

    /// Typecheck all children and set the type of the block
    /// to the type of the last child. TODO: noreturn?
    case NODE_BLOCK: {
      foreach_val (node, expr->block.children) {
        if (!typecheck_expression(ast, node))
          return false;

        if (node != vector_back(expr->block.children)) {
          if (node->kind == NODE_BINARY && node->binary.op == TK_EQ)
            ERR(node->source_location,
                "Comparison result unused. Did you mean to assign using %s?",
                token_type_to_string(TK_COLON_EQ));

          // If the function being called doesn't return void, it is being discarded.
          if (node->kind == NODE_CALL
              && !node->call.callee->type->function.attr_discardable
              && node->call.callee->type->function.return_type != t_void) {
            ERR(node->source_location,
                "Discarding return value of function that does not return void.");
          }
        }
      }
      expr->type = expr->block.children.size ? vector_back(expr->block.children)->type : t_void;
    } break;

    /// First, resolve the function. Then, typecheck all parameters
    /// and set the type to the return type of the callee.
    case NODE_CALL: {
      /// Builtins are handled separately.
      expr->call.intrinsic = intrinsic_kind(expr->call.callee);
      if (expr->call.intrinsic != INTRIN_COUNT) {
        if (!typecheck_intrinsic(ast, expr)) return false;
        break;
      }

      /// Resolve the function if applicable.
      Node *callee = expr->call.callee;
      if (!resolve_function(ast, callee)) return false;

      /// Typecheck the callee.
      if (!typecheck_expression(ast, callee)) return false;

      /// Callee must be a function or a function pointer.
      if (callee->type->kind == TYPE_FUNCTION) {
        /// Set the resolved function as the new callee.
        if (callee->kind != NODE_FUNCTION) {
          expr->call.callee = callee->funcref.resolved->val.node;
          callee = expr->call.callee;
          if (!typecheck_expression(ast, callee)) return false;
        }
      } else {
        /// Implicitly load the function pointer.
        if (callee->type->kind == TYPE_POINTER && callee->type->pointer.to->kind == TYPE_FUNCTION) {
          expr->call.callee = callee = ast_make_unary(ast, expr->source_location, TK_AT, false, callee);
          callee->parent = expr;
          if (!typecheck_expression(ast, callee)) return false;
        } else {
          ERR(expr->source_location, "Cannot call non-function type '%T'.", callee->type);
        }
      }

      /// Typecheck all arguments.
      foreach_val (param, expr->call.arguments)
        if (!typecheck_expression(ast, param))
          return false;

      /// Make sure we have the right number of arguments.
      if (expr->call.arguments.size != callee->type->function.parameters.size)
        ERR(callee->source_location, "Expected %Z arguments, got %Z.",
            callee->type->function.parameters.size, expr->call.arguments.size);

      /// Make sure all arguments are convertible to the parameter types.
      foreach_index(i, expr->call.arguments) {
        Parameter *param = &callee->type->function.parameters.data[i];
        Node *arg = expr->call.arguments.data[i];
        if (!convertible(param->type, arg->type)) ERR_NOT_CONVERTIBLE(arg->source_location, param->type, arg->type);
        if (!type_equals(param->type, arg->type)) {
          // Insert cast from argument type to parameter type, as they are convertible.
          Node *cast = ast_make_cast(ast, arg->source_location, param->type, arg);
          expr->call.arguments.data[i] = cast;
          if (!typecheck_expression(ast, cast)) return false;
        }
      }

      /// Set the type of the call to the return type of the callee.
      expr->type = callee->type->function.return_type;
    } break;

    /// Make sure a cast is even possible.
    case NODE_CAST: {
      Type *t_to = expr->type;
      // TO any incomplete type is DISALLOWED
      if (type_is_incomplete(t_to))
        ERR(t_to->source_location, "Cannot cast to incomplete type %T", t_to);

      if (!typecheck_expression(ast, expr->cast.value))
        return false;

      Type *t_from = expr->cast.value->type;

      // FROM any type T that is convertible TO type T' is ALLOWED
      if (convertible(t_to, t_from)) break;

      // FROM any incomplete type is DISALLOWED
      if (type_is_incomplete(t_from))
        ERR(expr->cast.value->source_location, "Cannot cast from an incomplete type %T", t_from);

      // FROM a non-lvalue expression TO a reference type is DISALLOWED
      if (type_is_reference(t_to) && !is_lvalue(expr->cast.value))
        ERR(expr->cast.value->source_location, "Cannot cast from a non-lvalue expression to reference type %T", t_to);

      // FROM any pointer type TO any pointer type is ALLOWED
      // TODO: Check base type size + alignment...
      if (type_is_pointer(t_from) && type_is_pointer(t_to)) break;
      // FROM any pointer type TO any integer type is ALLOWED
      if (type_is_pointer(t_from) && type_is_integer(t_to)) break;
      // FROM any integer type TO any integer type is ALLOWED
      if (type_is_integer(t_from) && type_is_integer(t_to)) break;

      // FROM an integer_literal type with value of zero TO any pointer type is ALLOWED
      if (t_from == t_integer_literal && expr->cast.value->literal.integer == 0 && type_is_pointer(t_to)) break;

      // FROM any integer type TO any pointer type is currently DISALLOWED, but very well may change
      if (type_is_integer(t_from) && type_is_pointer(t_to))
        ERR(expr->cast.value->source_location,
            "Cannot cast from an integer type %T to pointer type %T",
            t_from, t_to);

      // If a type has compatible alignment with and a size equal to the type
      // it is being cast to, there will be no memory errors, so why not allow
      // it? i.e. `u16[2] as u8[4]` or `u8[4] as s32`, etc.
      Type *t_from_base = type_strip_references(type_canonical(t_from));
      Type *t_to_base = type_strip_references(type_canonical(t_to));
      usz large_align = type_alignof(t_from_base);
      usz small_align = type_alignof(t_to_base);
      if (large_align < small_align) {
        usz tmp_align = large_align;
        large_align = small_align;
        small_align = tmp_align;
      }
      bool compatible_alignment = large_align % small_align == 0;
      if (!compatible_alignment) {
        print("Incompatible alignment\n");
      }
      if (type_sizeof(t_from_base) == type_sizeof(t_to_base) && compatible_alignment)
        break;

      ERR(expr->cast.value->source_location,
          "Casting from %T to %T is not supported by the typechecker\n"
          "  Open an issue with the current maintainers if you feel like this is not the proper behaviour.", t_from, t_to);
    }

    /// Binary expression. This is a complicated one.
    case NODE_BINARY: {
      /// Get this out of the way early.
      Node *const lhs = expr->binary.lhs;
      Node *const rhs = expr->binary.rhs;
      if (!typecheck_expression(ast, lhs)) return false;
      if (!typecheck_expression(ast, rhs)) return false;

      /// Typecheck the operator.
      switch (expr->binary.op) {
        default: ICE("Invalid binary operator '%s'.", token_type_to_string(expr->binary.op));

        /// The subscript operator is basically pointer arithmetic.
        case TK_LBRACK: {
          /// We can only subscript pointers and arrays, or references to either of those.
          Type *reference_stripped_lhs_type = type_strip_references(lhs->type);
          if (!type_is_pointer(reference_stripped_lhs_type) && !type_is_array(reference_stripped_lhs_type))
            ERR(lhs->source_location,
                "Cannot subscript non-pointer, non-array type '%T'.",
                lhs->type);

          /// The RHS has to be some sort of integer.
          if (!type_is_integer(rhs->type))
            ERR(rhs->source_location,
              "Cannot subscript with non-integer type '%T'.",
                rhs->type);

          if (rhs->kind == NODE_LITERAL && rhs->literal.type == TK_NUMBER) {
            if (type_is_array(reference_stripped_lhs_type) && reference_stripped_lhs_type->array.size <= rhs->literal.integer)
              ERR(rhs->source_location,
                  "Subscript %U out of bounds for array %T", rhs->literal.integer, reference_stripped_lhs_type);
          }

          /// The result of a subscript expression is a pointer to the
          /// start of the array, offset by the RHS.
          expr->type = ast_make_type_pointer(ast, lhs->source_location, reference_stripped_lhs_type->array.of);
        } break;

        /// All of these are basically the same when it comes to types.
        case TK_GT:
        case TK_LT:
        case TK_GE:
        case TK_LE:
        case TK_EQ:
        case TK_NE: {
          if ((type_is_integer(lhs->type) || type_is_pointer(lhs->type)) &&
              (type_is_integer(rhs->type) || type_is_pointer(rhs->type))) {
            /// TODO: Change this to bool if we ever add a bool type.
            expr->type = t_integer;
          } else {
            // Check for operator overloads, or replace binary operator with a call, or something...
            TODO("Handle binary operator %s with lhs type of %T and rhs type of %T\n",
                 token_type_to_string(expr->binary.op), lhs->type, rhs->type);
          }
        } break;

        /// Since pointer arithmetic is handled by the subscript operator,
        /// type checking for these is basically all the same.
        case TK_PLUS:
        case TK_MINUS:
        case TK_STAR:
        case TK_SLASH:
        case TK_PERCENT:
        case TK_SHL:
        case TK_SHR:
        case TK_AMPERSAND:
        case TK_PIPE:
        case TK_CARET: {
          if (type_is_integer(lhs->type) && type_is_integer(rhs->type)) {
            // Disallow (maybe warn?) when shifting more than/equal to size of type.
            if ((expr->binary.op == TK_SHL || expr->binary.op == TK_SHR) &&
                (rhs->kind == NODE_LITERAL && rhs->literal.type == TK_NUMBER && rhs->literal.integer >= (8 * type_sizeof(expr->binary.lhs->type))))
              ERR(expr->source_location,
                  "Cannot perform shift larger than size of underlying type %T (%Z is max).",
                  expr->binary.lhs->type, (8 * type_sizeof(expr->binary.lhs->type)) - 1);

            // Division/Modulus
            if (expr->binary.op == TK_SLASH || expr->binary.op == TK_PERCENT) {
              // Disallow divide by zero...
              if (rhs->kind == NODE_LITERAL && rhs->literal.type == TK_NUMBER && rhs->literal.integer == 0)
                ERR(expr->source_location, "Cannot perform division by zero.");
            }
            if (!type_equals(lhs->type, rhs->type)) {
              // Insert cast from rhs type to lhs type, as they are convertible but not equal.
              usz lhs_sz = type_sizeof(lhs->type);
              usz rhs_sz = type_sizeof(rhs->type);
              Node **smaller = lhs_sz < rhs_sz ? &expr->binary.lhs : &expr->binary.rhs;
              Node *larger = lhs_sz >= rhs_sz ? expr->binary.lhs : expr->binary.rhs;
              Node *cast = ast_make_cast(ast, (*smaller)->source_location, larger->type, *smaller);
              *smaller = cast;
              if (!typecheck_expression(ast, cast)) return false;
            }
          } else {
            // Check for operator overloads, or replace binary operator with a call, or something...
            TODO("Handle binary operator %s with lhs type of %T and rhs type of %T\n", token_type_to_string(expr->binary.op), lhs->type, rhs->type);
          }
          expr->type = lhs->type;
        } break;

        /// This is the complicated one.
        case TK_COLON_EQ:
        case TK_COLON_COLON:
          /// Make sure the lhs is an lvalue.
          if (!is_lvalue(lhs))
            ERR(lhs->source_location,
                "Cannot assign to non-lvalue type '%T'.",
                lhs->type);

          /// Make sure the rhs is convertible to the lhs.
          if (!convertible(lhs->type, rhs->type))
            ERR_NOT_CONVERTIBLE(rhs->source_location, lhs->type, rhs->type);

          /// Perform the conversion.
          /// FIXME: convertible() should do this instead.
          if (!type_equals(lhs->type, rhs->type)) {
            Node *cast = ast_make_cast(ast, rhs->source_location, type_strip_references(lhs->type), rhs);
            ASSERT(typecheck_expression(ast, cast));
            expr->binary.rhs = cast;
            cast->parent = expr;
          }

          expr->type = t_void;
          break;
      }
    } break;

    /// Here be dragons.
    case NODE_UNARY:
      if (!typecheck_expression(ast, expr->unary.value)) return false;
      switch (expr->unary.op) {
        default: ICE("Invalid unary operator '%s'.", token_type_to_string(expr->unary.op));

        /// We can only deference pointers.
        case TK_AT: {
          if (!type_is_pointer(expr->unary.value->type)) {
            ERR(expr->unary.value->source_location,
                "Argument of '@' must be a pointer.");
          }

          Type *pointee_type = type_canonical(expr->unary.value->type->pointer.to);
          if (!pointee_type) {
            ERR(expr->unary.value->source_location,
                "Cannot dereference incomplete pointer type %T",
                expr->unary.value->type->pointer.to);
          }

          /// The result type of a dereference is the pointee.
          expr->type = expr->unary.value->type->pointer.to;
          } break;

        /// Address of lvalue.
        case TK_AMPERSAND:
          if (!is_lvalue(expr->unary.value))
            ERR(expr->unary.value->source_location,
              "Argument of '&' must be an lvalue.");

          expr->type = ast_make_type_pointer(ast, expr->source_location, expr->unary.value->type);
          break;

        /// One’s complement negation.
        case TK_TILDE:
          if (!type_is_integer(expr->unary.value->type))
            ERR(expr->unary.value->source_location,
              "Argument of '~' must be an integer.");

          expr->type = expr->unary.value->type;
          break;
      }
      break;

    case NODE_LITERAL: {
      switch (expr->literal.type) {
      case TK_NUMBER: expr->type = t_integer_literal; break;
      case TK_STRING: {
        string s = ast->strings.data[expr->literal.string_index];
        expr->type = ast_make_type_array(ast, expr->source_location, t_byte, s.size + 1);
      } break;
      case TK_LBRACK:
        if (!expr->literal.compound.size) {
          ERR(expr->source_location,
              "An array literal must have elements within it, as a zero-sized array makes no sense!");
        }
        Type *type = NULL;
        foreach_val (node, expr->literal.compound) {
          if (!typecheck_expression(ast, node)) return false;
          if (type && !convertible(type, node->type)) {
            ERR(node->source_location,
                "Every expression within an array literal must be convertible to the same type: %T.",
                type);
          }
          if (!type) type = node->type;
        }
        expr->type = ast_make_type_array(ast, expr->source_location, type, expr->literal.compound.size);
        break;
      default:
        TODO("Literal type %s.", token_type_to_string(expr->literal.type));
      }
    } break;

    /// The type of a variable reference is the type of the variable.
    case NODE_VARIABLE_REFERENCE:
      if (!typecheck_expression(ast, expr->var->val.node)) return false;
      expr->type = expr->var->val.node->type;
      break;

    /// The type of a structure declaration is the type of the struct.
    case NODE_STRUCTURE_DECLARATION:
      return typecheck_type(ast, expr->struct_decl->val.type);

    /// The type of a structure declaration is the type of the struct.
    case NODE_MEMBER_ACCESS: {
      // TODO: Auto dereference left hand side of pointers.

      // FIXME: This is slightly scuffed to call this "_struct" when it
      // now also may represent modules.
      if (!typecheck_expression(ast, expr->member_access.struct_)) return false;

      if (expr->member_access.struct_->kind == NODE_MODULE_REFERENCE) {
        /*
        print("Lookup %S in module %S\n",
              as_span(expr->member_access.ident),
              as_span(expr->member_access.struct_->module_ref.ast->module_name));
        */

      Module *module = NULL;
        foreach_val (m, ast->imports) {
          if (string_eq(m->module_name, expr->member_access.struct_->module_ref.ast->module_name)) {
            module = m;
            break;
          }
        }
        if (!module) ERR((loc){0}, "Attempt to reference module which has not been imported!");

        Node *found = NULL;
        foreach_val (n, module->exports) {
          string *name = NULL;
          if (n->kind == NODE_DECLARATION)
            name = &n->declaration.name;
          else if (n->kind == NODE_FUNCTION_REFERENCE)
            name = &n->funcref.name;
          else ICE("Unexpected node type exported by module");
          if (string_eq(*name, expr->member_access.ident)) {
            found = n;
            break;
          }
        }
        if (!found)
          ERR(expr->source_location, "Undefined reference to \"%S\" in module %S",
              as_span(expr->member_access.ident),
              as_span(expr->member_access.struct_->module_ref.ast->module_name));

        if (found->kind == NODE_DECLARATION) {
          expr->kind = NODE_VARIABLE_REFERENCE;
          expr->var = calloc(1, sizeof(Symbol));
          expr->var->kind = SYM_VARIABLE;
          expr->var->name = string_dup(found->declaration.name);
          expr->var->val.node = found;
          expr->type = found->type;
        } else if (found->kind == NODE_FUNCTION_REFERENCE) {
          expr->kind = NODE_FUNCTION_REFERENCE;
          expr->funcref.name = string_dup(found->funcref.name);
          expr->funcref.resolved = found->funcref.resolved;
          expr->funcref.scope = found->funcref.scope;
          expr->type = found->type;
        } else ICE("Unrecognised deseraialised module declaration kind %d\n", (int)found->kind);

        return true;

      } else {
        // Ensure struct_ is of struct type.
        Type *struct_type = type_canonical(expr->member_access.struct_->type);
        if (!struct_type || struct_type->kind != TYPE_STRUCT)
          ERR(expr->member_access.struct_->source_location,
              "Cannot access member of type %T", struct_type);

        Member *member = vector_find_if(m, struct_type->structure.members,
                         string_eq(m->name, expr->member_access.ident));
        if (!member)
          ERR(expr->source_location,
              "Cannot access member \"%S\" that does not exist in \"%S\", an instance of %T",
              expr->member_access.ident, expr->member_access.struct_->struct_decl->name, struct_type);

        expr->member_access.member = member;
        expr->type = member->type;

        return true;
      }
    }

    case NODE_FOR: {
      if (!typecheck_expression(ast, expr->for_.init) ||
          !typecheck_expression(ast, expr->for_.condition) ||
          !typecheck_expression(ast, expr->for_.iterator) ||
          !typecheck_expression(ast, expr->for_.body))
        return false;
      // FIXME: Should be t_bool
      if (!convertible(t_integer, expr->for_.condition->type)) {
        ERR(expr->for_.condition->source_location,
            "Type of condition expression of for loop %T is not convertible to %T",
            expr->for_.condition->type, t_integer);
      }

      expr->type = t_void;
      return true;
    }

    case NODE_RETURN: {
      // Get function we are returning from.
      // TODO: It would be more efficient to cache function type in return AST node while parsing.
      Node *func = expr->parent;
      while (func && func->kind != NODE_FUNCTION) func = func->parent;

      // Ensure return nodes within void return-type functions have no value.
      if (expr->return_.value && func && func->type->function.return_type == t_void)
        ERR(expr->return_.value->source_location,
            "An expression must not follow `return` in a function returning void.");

      if (expr->return_.value && !typecheck_expression(ast, expr->return_.value))
        return false;

      expr->type = expr->return_.value ? expr->return_.value->type : t_void;
      return true;
    }

    /// Resolve the function reference and typecheck the function.
    case NODE_FUNCTION_REFERENCE:
      if (!resolve_function(ast, expr)) return false;
      if (!typecheck_expression(ast, expr->funcref.resolved->val.node)) return false;
      ast_replace_node(ast, expr, expr->funcref.resolved->val.node);
      break;

  }

  /// If this is a pointer type, make sure it doesn’t point to an incomplete type.
  Type *base = expr->type;
  while (base && type_is_pointer(base)) base = base->pointer.to;
  if (base && type_is_pointer(expr->type /** (!) **/) && type_is_incomplete(base))
    ERR(expr->source_location,
        "Cannot use pointer to incomplete type '%T'.",
        expr->type->pointer.to);

  /// Done.
  return true;
}
