#include <ast.h>
#include <codegen.h>
#include <error.h>
#include <locale.h>
#include <parser.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <typechecker.h>
#include <platform.h>
#include <utils.h>
#include <vector.h>

// TODO: TMP
#include <module.h>
#include <codegen/coff.h>
#include <codegen/elf.h>

static void print_usage(char **argv) {
  print("\nUSAGE: %s [FLAGS] [OPTIONS] <path to file to compile>\n", 0[argv]);
  print("Flags:\n"
        "   `-h`, `--help`      :: Show this help and usage information.\n"
        "   `-as`, `--archs`    :: List acceptable architectures.\n"
        "   `-ts`, `--targets`  :: List acceptable targets.\n"
        "   `-ccs`, `--callings`:: List acceptable calling conventions.\n"
        "   `--syntax-only      :: Exit just after parsing, before semantic analysis.\n"
        "   `--print-ast        :: Print the syntax tree.\n"
        "   `--print-scopes     :: Print the scope tree.\n"
        "   `--print-ir`        :: Print the intermediate representation.\n"
        "   `--annotate-code    :: Emit comments in generated code.\n"
        "   `-O`, `--optimize`  :: Optimize the generated code.\n"
        "   `-v`, `--verbose`   :: Print out more information.\n");
  print("Options:\n"
        "    `-o`, `--output`   :: Set the output filepath to the one given.\n"
        "    `-a`, `--arch`     :: Set the output architecture to the one given.\n"
        "    `-t`, `--target`   :: Set the output target to the one given.\n"
        "    `-cc`, `--calling` :: Set the calling convention to the one given.\n"
        "   `--dot-cfg <func>`  :: Print the control flow graph of a function in DOT format and exit.\n"
        "   `--dot-dj <func>`   :: Print the DJ-graph of a function in DOT format and exit.\n"
        "    `-L`               :: Check for modules within the given directory.\n"
        "    `--colours`        :: Set whether to use colours in diagnostics.\n"
        "Anything other arguments are treated as input filepaths (source code).\n");
}

int input_filepath_index = -1;
int output_filepath_index = -1;
CodegenArchitecture output_arch = ARCH_DEFAULT;
CodegenTarget output_target = TARGET_DEFAULT;
enum CodegenCallingConvention output_calling_convention = CG_CALL_CONV_DEFAULT;

int verbosity = 0;
int optimise = 0;
bool debug_ir = false;
bool print_ast = false;
bool syntax_only = false;
bool print_scopes = false;
bool prefer_using_diagnostics_colours = true;
bool colours_blink = false;
bool annotate_code = false;
bool print_ir2 = false;
bool print_dot_cfg = false;
bool print_dot_dj = false;
const char* print_dot_function = NULL;
Vector(string) search_paths = {};

static void print_acceptable_architectures() {
  STATIC_ASSERT(ARCH_COUNT == 2, "Exhaustive handling of architectures when printing out all available");
  print("Acceptable architectures include:\n"
         " -> default\n"
         " -> x86_64\n");
}

static void print_acceptable_targets() {
  STATIC_ASSERT(TARGET_COUNT == 6, "Exhaustive handling of targets when printing out all available");
  print("Acceptable targets include:\n"
        " -> default\n"
        " -> asm, assembly\n"
        " -> asm:intel\n"
        " -> llvm -- LLVM IR\n"
        " -> obj, object  --  system default object file format\n"
        " -> elf_object\n"
        " -> coff_object\n");
}

static void print_acceptable_calling_conventions() {
  print("Acceptable calling conventions include:\n"
         " -> default\n"
         " -> SYSV, LINUX\n"
         " -> MSWIN\n");
}

static void print_acceptable_colour_settings() {
  print("Acceptable values for `--colours` include:\n"
         " -> auto\n"
         " -> always\n"
         " -> blink\n"
         " -> never\n");
}

/// @return Zero if everything goes well, otherwise return non-zero value.
static int handle_command_line_arguments(int argc, char **argv) {
  /// Default settings.
  prefer_using_diagnostics_colours = platform_isatty(fileno(stdout));

  for (int i = 1; i < argc; ++i) {
    char *argument = i[argv];

    //print("argument %d: \"%s\"\n", i, argument);

    if (strcmp(argument, "-h") == 0
        || strcmp(argument, "--help") == 0) {
      print_usage(argv);
      exit(0);
    } else if (strcmp(argument, "--print-ir2") == 0) {
      print_ir2 = true; /// Print high-level IR only and exit w/o codegen w/ code 42.
    } else if (strcmp(argument, "--print-ir") == 0) {
      debug_ir = true;
    } else if (strcmp(argument, "--print-ast") == 0) {
      print_ast = true;
    } else if (strcmp(argument, "--print-scopes") == 0) {
      print_scopes = true;
    } else if (strcmp(argument, "--syntax-only") == 0) {
      syntax_only = true;
    } else if (strcmp(argument, "--annotate-code") == 0) {
      annotate_code = true;
    } else if (strcmp(argument, "--dot-cfg") == 0) {
      print_dot_cfg = true;
      if (++i >= argc)
        ICE("Expected target after command line argument %s", argument);
      print_dot_function = i[argv]; /// Note: Copilot autocompleted this and I’m leaving it like that lol.
    } else if (strcmp(argument, "--dot-dj") == 0) {
      print_dot_dj = true;
      if (++i >= argc)
        ICE("Expected target after command line argument %s", argument);
      print_dot_function = i[argv]; /// Note: Copilot autocompleted this and I’m leaving it like that lol.
    } else if (strcmp(argument, "-O") == 0
               || strcmp(argument, "--optimise") == 0) {
      optimise = 1;
    }  else if (strcmp(argument, "-v") == 0
               || strcmp(argument, "--verbose") == 0) {
      verbosity = 1;
    } else if (strcmp(argument, "-as") == 0 || strcmp(argument, "--archs") == 0) {
      print_acceptable_architectures();
      exit(0);
    } else if (strcmp(argument, "-a") == 0 || strcmp(argument, "--arch") == 0) {
      i++;
      if (i >= argc)
        ICE("Expected architecture after command line argument %s", argument);

      /// Anything that starts w/ `-` is treated as a command line argument.
      /// If the user has a filepath that starts w/ `-...`, then they should use
      /// `./-...` instead.
      if (0[i[argv]] == '-') {
        ICE("Expected architecture after command line argument %s\n"
               "Instead, got what looks like another command line argument.\n"
               " -> \"%s\"", argument, argv[i]);
      }
      STATIC_ASSERT(ARCH_COUNT == 2, "Exhaustive handling of architecture count in command line argument parsing");
      if (strcmp(argv[i], "default") == 0) {
        output_arch = ARCH_DEFAULT;
      } else if (strcmp(argv[i], "x86_64_gas") == 0) {
        output_arch = ARCH_X86_64;
      } else {
        print("Expected architecture after command line argument %s\n"
               "Instead, got unrecognized: \"%s\".\n", argument, argv[i]);
        print_acceptable_architectures();
        return 1;
      }
    } else if (strcmp(argument, "-ts") == 0 || strcmp(argument, "--targets") == 0) {
      print_acceptable_targets();
      exit(0);
    } else if (strcmp(argument, "-t") == 0 || strcmp(argument, "--target") == 0) {
      i++;
      if (i >= argc)
        ICE("Expected target after command line argument %s", argument);

      /// Anything that starts w/ `-` is treated as a command line argument.
      /// If the user has a filepath that starts w/ `-...`, then they should use
      /// `./-...` instead.
      if (0[i[argv]] == '-') {
        ICE("Expected target after command line argument %s\n"
               "Instead, got what looks like another command line argument.\n"
               " -> \"%s\"", argument, argv[i]);
      }
      STATIC_ASSERT(TARGET_COUNT == 6, "Exhaustive handling of target count in command line argument parsing");
      if (strcmp(argv[i], "default") == 0) {
        output_target = TARGET_DEFAULT;
      } else if (strcmp(argv[i], "asm") == 0 || strcmp(argv[i], "assembly") == 0) {
        output_target = TARGET_GNU_ASM_ATT;
      } else if (strcmp(argv[i], "asm:intel") == 0) {
        output_target = TARGET_GNU_ASM_INTEL;
      } else if (strcmp(argv[i], "llvm") == 0) {
        output_target = TARGET_LLVM;
      } else if (strcmp(argv[i], "obj") == 0 || strcmp(argv[i], "object") == 0) {
#ifdef _WIN32
        output_target = TARGET_COFF_OBJECT;
#else
        output_target = TARGET_ELF_OBJECT;
#endif
      } else if (strcmp(argv[i], "elf_object") == 0) {
        output_target = TARGET_ELF_OBJECT;
      } else if (strcmp(argv[i], "coff_object") == 0) {
        output_target = TARGET_COFF_OBJECT;
      } else {
        print("Expected architecture after command line argument %s\n"
               "Instead, got unrecognized: \"%s\".\n", argument, argv[i]);
        print_acceptable_architectures();
        return 1;
      }
    } else if (strcmp(argument, "-o") == 0
               || strcmp(argument, "--output") == 0) {
      i++;
      if (i >= argc) {
        ICE("Expected filepath after output command line argument");
      }
      /// Anything that starts w/ `-` is treated as a command line argument.
      /// If the user has a filepath that starts w/ `-...`, then they should use
      /// `./-...` instead.
      if (0[i[argv]] == '-') {
        ICE("Expected filepath after output command line argument\n"
               "Instead, got what looks like another command line argument.\n"
               " -> \"%s\"", argv[i]);
      }
      output_filepath_index = i;
    } else if (strcmp(argument, "--colours") == 0 || strcmp(argument, "--colors") == 0) {
      i++;
      if (i >= argc) {
        fprint(stderr, "Error: Expected option value after `--colours`\n");
        print_acceptable_colour_settings();
        exit(1);
      }
      if (strcmp(argv[i], "auto") == 0) {
        prefer_using_diagnostics_colours = platform_isatty(fileno(stdout));
      } else if (strcmp(argv[i], "never") == 0) {
        prefer_using_diagnostics_colours = false;
      } else if (strcmp(argv[i], "blink") == 0) {
        prefer_using_diagnostics_colours = true;
        colours_blink = true;
      } else if (strcmp(argv[i], "always") == 0) {
        prefer_using_diagnostics_colours = true;
      } else {
        print("Expected colour option after colour option command line argument\n"
               "Instead, got unrecognized: \"%s\".\n", argv[i]);
        print_acceptable_colour_settings();
        return 1;
      }
    } else if (strcmp(argument, "-ccs") == 0 || strcmp(argument, "--callings") == 0) {
      print_acceptable_calling_conventions();
      exit(0);
    } else if (strcmp(argument, "-cc") == 0 || strcmp(argument, "--calling") == 0) {
      i++;
      if (i >= argc) {
        ICE("Expected calling convention after command line argument %s", argument);
      }
      if (*argv[i] == '-') {
        ICE("Expected calling convention after command line argument %s\n"
            "Instead, got what looks like another command line argument.\n"
            " -> \"%s\"", argument, argv[i]);
      }
      STATIC_ASSERT(CG_CALL_CONV_COUNT == 2, "Exhaustive handling of calling conventions in command line argument parsing");
      if (strcmp(argv[i], "default") == 0) {
        output_calling_convention = CG_CALL_CONV_DEFAULT;
      } else if (strcmp(argv[i], "MSWIN") == 0) {
        output_calling_convention = CG_CALL_CONV_MSWIN;
      } else if (strcmp(argv[i], "SYSV") == 0 || strcmp(argv[i], "LINUX") == 0) {
        output_calling_convention = CG_CALL_CONV_SYSV;
      } else {
        print("Expected calling convention after command line argument %s\n"
              "Instead, got unrecognized: \"%s\".\n", argument, argv[i]);
        print_acceptable_calling_conventions();
        return 1;
      }
    } else if (strcmp(argument, "-L") == 0) {
      i++;
      if (i >= argc) {
        fprint(stderr, "Error: Expected directory path after `-L`\n");
        print_acceptable_colour_settings();
        exit(1);
      }
      vector_push(search_paths, string_create(argv[i]));
    } else if (strcmp(argument, "--aluminium") == 0) {
#     if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
      // Windows
      system("start https://www.youtube.com/watch?v=dQw4w9WgXcQ");
#     elif __APPLE__
      // Apple (iOS, OS X, watchOS...)
      system("open https://www.youtube.com/watch?v=dQw4w9WgXcQ");
#     elif __linux__ || __unix__
      // Linux or unix-based
      system("xdg-open https://www.youtube.com/watch?v=dQw4w9WgXcQ");
#     endif
    } else {
      if (input_filepath_index == -1) input_filepath_index = i;
      else ICE("Error: Unrecognized command line argument: \"%s\"", argument);
    }
  }
  return 0;
}

// TODO: Dear god, please move this.
span grab_section_reference_elf(span object_file, const char *section_name) {
  // TODO: Check if file is actually big enough to be an ELF file.
  elf64_header* header = (elf64_header*)object_file.data;
  // TODO: Validate header.
  if (header->e_machine != EM_X86_64)
    ICE("ELF has invalid machine type");

  // Offset into file to find section header table.
  // TODO: Validate offset within file.
  elf64_shdr* section_header = (elf64_shdr*)(object_file.data + header->e_shoff);
  elf64_shdr* string_table_header = section_header + header->e_shstrndx;
  span string_table = {
    object_file.data + string_table_header->sh_offset,
    string_table_header->sh_size
  };
  for (size_t i = 0; i < header->e_shnum; ++i) {
    const char *sh_name = string_table.data + section_header->sh_name;
    if (strcmp(sh_name, section_name) == 0) {
      span section_reference = {0};
      section_reference.data = object_file.data + section_header->sh_offset;
      section_reference.size = section_header->sh_size;
      return section_reference;
    }
    section_header++;
  }
  ICE("Could not find section %s within ELF object file", section_name);
}

span grab_section_reference_coff(span object_file, const char *section_name) {
  ASSERT(object_file.data, "Invalid argument");
  ASSERT(section_name, "Invalid argument");
  const size_t section_name_length = strlen(section_name);

  coff_header *header = (coff_header*)object_file.data;
  // TODO: Validate header.

  span string_table = {0};
  // The string table starts after the symbol table.
  string_table.data = object_file.data + header->f_symptr + ((usz)header->f_nsyms * sizeof(coff_symbol_entry));
  string_table.size = *(uint32_t*)string_table.data;

  coff_section_header *section_header = (coff_section_header*)(object_file.data + sizeof(*header));
  for (size_t i = 0; i < header->f_nscns; ++i, ++section_header) {
    if (section_name_length > sizeof(section_header->s_name)) {
      if (section_header->s_name[0] != '/') continue;
      // TODO: Parse unsigned decimal integer from the last seven digits of s_name
      char *end = NULL;
      usz name_offset_in_string_table = (usz)strtoull(section_header->s_name + 1, &end, 10);
      // TODO: Use parsed integer as offset into string table to find name.
      if (strncmp(string_table.data + name_offset_in_string_table, section_name, section_name_length) == 0) {
        span section_reference = {0};
        section_reference.data = object_file.data + section_header->s_scnptr;
        section_reference.size = (usz)section_header->s_size;
        return section_reference;
      }
    } else {
      if (strncmp(section_header->s_name, section_name, 8) == 0) {
        span section_reference = {0};
        section_reference.data = object_file.data + section_header->s_scnptr;
        section_reference.size = (usz)section_header->s_size;
        return section_reference;
      }
    }
  }
  ICE("Could not find section %s within COFF object file", section_name);
}

span grab_section_reference(span object_file, const char *section_name) {
  if (string_starts_with(object_file, literal_span("\x7f""ELF")))
    return grab_section_reference_elf(object_file, section_name);
  return grab_section_reference_coff(object_file, section_name);
}

// TODO: MOVE THIS
/// Return the file extension expected by a given target.
const char *target_extension(CodegenTarget target) {
  STATIC_ASSERT(TARGET_COUNT == 6, "Exhaustive handling of codegen targets while returning expected file extension");
  switch (target) {
  case TARGET_GNU_ASM_ATT: FALLTHROUGH;
  case TARGET_GNU_ASM_INTEL: return "s";

  case TARGET_LLVM: return "ll";
  case TARGET_COFF_OBJECT: return "obj";
  case TARGET_ELF_OBJECT: return "o";

  default:
  case TARGET_NONE:
  case TARGET_COUNT: UNREACHABLE();
  }
}

int main(int argc, char **argv) {
  primitive_types[0] = t_integer;
  primitive_types[1] = t_void;
  primitive_types[2] = t_byte;
  primitive_types[3] = NULL;

  platform_init();

  if (argc < 2) {
    print_usage(argv);
    return 0;
  }

  int status = handle_command_line_arguments(argc, argv);
  if (status) return status;
  if (input_filepath_index == -1) {
    print("Input file path was not provided.");
    print_usage(argv);
    return 1;
  }

  thread_use_colours = prefer_using_diagnostics_colours;

  const char *infile = argv[input_filepath_index];

  string output_filepath = {0};
  if (output_filepath_index != -1)
    output_filepath = string_create(argv[output_filepath_index]);
  else {
    // Create output filepath from infile

    // Copy input filepath starting at last path separator.

    // TODO: Strip path separator from end, if present.

    // Find last occurence of path separator in input filepath.
    char *last_path_separator = strrstr((char*)infile, PLATFORM_PATH_SEPARATOR);
    // If there are no occurences of a path separator, just use the entire thing.
    if (!last_path_separator) last_path_separator = (char*)infile;
    else ++last_path_separator; // Yeet path separator.

    string_buffer path = {0};
    for (const char *c = last_path_separator; *c; ++c)
      vector_push(path, *c);
    // Strip file extension
    char *last_dot = strrchr(path.data, '.');
    if (last_dot) {
      usz last_dot_index = (usz)(last_dot - path.data);
      path.size = last_dot_index;
    }

    // Add extension based on target
    format_to(&path, ".%s", target_extension(output_target));
    string_buf_zterm(&path);

    // Move string buffer `path` to `output_filepath` string.
    output_filepath.data = path.data;
    output_filepath.size = path.size;
    path.data = NULL;
    path.size = 0;

  }
  size_t len = strlen(infile);
  bool ok = false;
  string s = platform_read_file(infile, &ok);
  if (!ok) ICE("%S", s);

  /// The input is an IR file.
  if (len >= 3 && memcmp(infile + len - 3, ".ir", 3) == 0) {
    ASSERT(s.data);

    TODO("Development of IR parser and codegen is severely behind right now.");

    if (!codegen(
      LANG_IR,
      output_arch,
      output_target,
      output_calling_convention,
      infile,
      output_filepath.data,
      NULL,
      s
     )) {
      exit(1);
     }

    free(s.data);
  }

  /// The input is an Intercept file.
  else {
    /// Parse the file.
    Module *ast = parse(as_span(s), infile);
    if (!ast) exit(1);

    // If this is a module and the user did not provide output
    // filename, use the module name.
    if (ast->is_module && output_filepath_index == -1) {
      // TODO: "contains" isn't the best check, but I don't want to write a
      // path parser to get the base name right now.
      if (!strstr(infile, ast->module_name.data)) {
        issue_diagnostic(DIAG_WARN, infile, as_span(s), (loc){0},
                         "Source file name does not match name of exported module: %s doesn't contain %S",
                         infile, ast->module_name);
      }

      free(output_filepath.data);

      // Construct output path from module name
      string_buffer path = {0};
      vector_append(path, ast->module_name);
      format_to(&path, ".%s", target_extension(output_target));

      // MOVE path -> output_filepath
      output_filepath.data = path.data;
      output_filepath.size = path.size;
      path.data = NULL;
      path.size = 0;
    }

    /// Print if requested.
    if (syntax_only) {
      if (print_ast) ast_print(stdout, ast);
      if (print_scopes) ast_print_scope_tree(stdout, ast);
      goto done;
    }

    // Resolve imported modules
    vector_push(search_paths, string_create("."));

    Vector(span) extensions_to_try = {0};
#ifdef _WIN32
      vector_push(extensions_to_try, literal_span(".obj"));
      vector_push(extensions_to_try, literal_span(".o"));
#else
      vector_push(extensions_to_try, literal_span(".o"));
      vector_push(extensions_to_try, literal_span(".obj"));
#endif

    foreach_index (i, ast->imports) {
      Module *import = ast->imports.data[i];

      string module_object = {0};
      bool success = false;

      string_buffer search_path = {0};
      const span dir_sep = literal_span("/");
      foreach_index(j, search_paths) {
        vector_clear(search_path);
        vector_append(search_path, search_paths.data[j]);
        // TODO: Only do this if path doesn't already end with dir sep.
        vector_append(search_path, dir_sep);
        vector_append(search_path, import->module_name);
        usz module_path_length_without_extension = search_path.size;
        foreach (extension, extensions_to_try) {
          search_path.size = module_path_length_without_extension;
          vector_append(search_path, *extension);
          //print("Looking for module %S at %S\n", as_span(import->module_name), as_span(search_path));
          module_object = platform_read_file(search_path.data, &success);
          if (success) break;
        }
        if (success) break;
      }

      ASSERT(success, "Could not find module description for module %S", import->module_name);

      print("Resolved module %S at path %S\n", import->module_name, as_span(search_path));

      vector_delete(search_path);

      span metadata = grab_section_reference(as_span(module_object), INTC_MODULE_SECTION_NAME);
      ast->imports.data[i] = deserialise_module(metadata);
      ast->imports.data[i]->module_name = import->module_name;

      foreach_val (export, ast->imports.data[i]->exports) {
        if (export->kind == NODE_FUNCTION_REFERENCE) {
          Scope *global_scope = vector_front(ast->scope_stack);
          Symbol *func_sym = scope_find_or_add_symbol(global_scope, SYM_FUNCTION, as_span(export->funcref.name), true);
          /// FIXME: Should probably create function in imported module?
          func_sym->val.node = ast_make_function(ast, (loc){0}, export->type, LINKAGE_IMPORTED, (Nodes){0}, NULL, as_span(export->funcref.name));
          export->funcref.scope = global_scope;
          export->funcref.resolved = func_sym;
        }
      }

      free(module_object.data);
    }
    vector_delete(extensions_to_try);

    /// Perform semantic analysis on program expressions.
    ok = typecheck_expression(ast, ast->root);
    if (!ok) exit(2);

    /// Print if requested.
    if (print_ast) ast_print(stdout, ast);
    if (print_scopes) ast_print_scope_tree(stdout, ast);

    /// Generate code.
    if (!codegen(
      LANG_FUN,
      output_arch,
      output_target,
      output_calling_convention,
      infile,
      output_filepath.data,
      ast,
      (string){0}
    )) exit(3);

  done:
    ast_free(ast);
  }

  /// Free the input file contents.
  free(s.data);

  /// Done!
  if (verbosity) print("\nGenerated code at output filepath \"%S\"\n", output_filepath);

  /// Free the output filepath.
  free(output_filepath.data);
}
