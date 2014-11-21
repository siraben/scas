#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "log.h"
#include "list.h"
#include "enums.h"
#include "errors.h"
#include "assembler.h"
#include "linker.h"
#include "expression.h"

struct {
	char *arch;
	jobs_t jobs;
	int explicit_export;
	int explicit_import;
	output_type_t output_type;
	list_t *input_files;
	char *output_file;
	char *listing_file;
	char *symbol_file;
	char *include_path;
	char *linker_script;
	int verbosity;
	int automatic_relocation;
	int merge_only;
} runtime;

void init_runtime() {
	runtime.arch = "z80";
	runtime.jobs = LINK | ASSEMBLE;
	runtime.explicit_import = 1;
	runtime.explicit_export = 0;
	runtime.output_type = EXECUTABLE;
	runtime.input_files = create_list();
	runtime.output_file = NULL;
	runtime.listing_file = NULL;
	runtime.symbol_file = NULL;
	runtime.include_path = getenv("SCAS_PATH");
	if (!runtime.include_path) {
		runtime.include_path = malloc(3);
		strcpy(runtime.include_path, "./");
	}
	runtime.linker_script = NULL;
	runtime.verbosity = L_SILENT;
	runtime.automatic_relocation = 0;
}

void validate_runtime() {
	if (runtime.input_files->length == 0) {
		scas_abort("No input files given");
	}
	if (runtime.output_file == NULL) {
		/* Auto-assign an output file name */
		const char *ext;
		if ((runtime.jobs & LINK) == LINK) {
			ext = ".bin";
		} else {
			ext = ".o";
		}
		runtime.output_file = malloc(strlen(runtime.input_files->items[0]) + strlen(ext) + 1);
		memcpy(runtime.output_file, runtime.input_files->items[0], strlen(runtime.input_files->items[0]));
		int i = strlen(runtime.output_file);
		while (runtime.output_file[--i] != '.' && i != 0);
		if (i == 0) {
			i = strlen(runtime.output_file);
		}
		int j;
		for (j = 0; j < sizeof(ext); j++) {
			runtime.output_file[i + j] = ext[j];
		}
		scas_log(L_DEBUG, "Assigned output file name to %s", runtime.output_file);
	}
}

void parse_arguments(int argc, char **argv) {
	int i;
	for (i = 1; i < argc; ++i) {
		if (argv[i][0] == '-' && argv[i][1] != '\0') {
			if (strcmp("-o", argv[i]) == 0 || strcmp("--output", argv[i]) == 0) {
				runtime.output_file = argv[++i];
			} else if (strcmp("-i", argv[i]) == 0 || strcmp("--input", argv[i]) == 0) {
				list_add(runtime.input_files, argv[++i]);
			} else if (strcmp("-l", argv[i]) == 0 || strcmp("--link", argv[i]) == 0) {
				runtime.jobs = LINK;
			} else if (strcmp("-O", argv[i]) == 0 || strcmp("--object", argv[i]) == 0) {
				runtime.jobs = ASSEMBLE;
			} else if (strcmp("-m", argv[i]) == 0 || strcmp("--merge", argv[i]) == 0) {
				runtime.merge_only = 1;
			} else if (argv[i][1] == 'I' || strcmp("--include", argv[i]) == 0) {
				char *path;
				if (argv[i][1] == 'I' && argv[i][2] != 0) {
					// -I/path/goes/here
					path = argv[i] + 2;
				} else {
					// [-I | --include] path/goes/here
					path = argv[++i];
				}
				int l = strlen(runtime.include_path);
				runtime.include_path = realloc(runtime.include_path, l + strlen(path) + 2);
				strcat(runtime.include_path, ":");
				strcat(runtime.include_path, path);
			} else if (strcmp("-r", argv[i]) == 0 || strcmp("--relocatable", argv[i]) == 0) {
				runtime.automatic_relocation = 1;
			} else if (strcmp("-e", argv[i]) == 0 || strcmp("--export-explicit", argv[i]) == 0) {
				runtime.explicit_export = 1;
			} else if (strcmp("-n", argv[i]) == 0 || strcmp("--no-implicit-symbols", argv[i]) == 0) {
				runtime.explicit_import = 0;
			} else if (argv[i][1] == 'v') {
				int j;
				for (j = 1; argv[i][j] != '\0'; ++j) {
					if (argv[i][j] == 'v') {
						runtime.verbosity++;
					} else {
						scas_abort("Invalid option %s", argv[i]);
					}
				}
			} else {
				scas_abort("Invalid option %s", argv[i]);
			}
		} else {
			if (runtime.output_file != NULL || i != argc - 1 || runtime.input_files->length == 0) {
				scas_log(L_INFO, "Added input file '%s'", argv[i]);
				list_add(runtime.input_files, argv[i]);
			} else if (runtime.output_file == NULL && i == argc - 1) {
				runtime.output_file = argv[i];
			}
		}
	}
}

instruction_set_t *find_inst() {
	const char *sets_dir = INSTRUCTION_SET_PATH;
	const char *ext = ".tab";
	FILE *f = fopen(runtime.arch, "r");
	if (!f) {
		char *path = malloc(strlen(runtime.arch) + strlen(sets_dir) + strlen(ext) + 1);
		strcpy(path, sets_dir);
		strcat(path, runtime.arch);
		strcat(path, ext);
		f = fopen(path, "r");
		free(path);
		if (!f) {
			scas_abort("Unknown architecture: %s", runtime.arch);
		}
	}
	instruction_set_t *set = load_instruction_set(f);
	fclose(f);
	return set;
}

list_t *split_include_path() {
	list_t *list = create_list();
	int i, j;
	for (i = 0, j = 0; runtime.include_path[i]; ++i) {
		if (runtime.include_path[i] == ':' || runtime.include_path[i] == ';') {
			char *s = malloc(i - j + 1);
			strncpy(s, runtime.include_path + j, i - j);
			s[i - j] = '\0';
			j = i + 1;
			list_add(list, s);
		}
	}
	char *s = malloc(i - j + 1);
	strncpy(s, runtime.include_path + j, i - j);
	s[i - j] = '\0';
	list_add(list, s);
	return list;
}

int main(int argc, char **argv) {
	init_runtime();
	parse_arguments(argc, argv);
	init_log(runtime.verbosity);
	validate_runtime();
	instruction_set_t *instruction_set = find_inst();
	scas_log(L_INFO, "Loaded instruction set: %s", instruction_set->arch);
	list_t *include_path = split_include_path();
	list_t *errors = create_list();
	list_t *warnings = create_list();

	list_t *objects = create_list();
	if ((runtime.jobs & ASSEMBLE) == ASSEMBLE) {
		int i;
		for (i = 0; i < runtime.input_files->length; ++i) {
			scas_log(L_INFO, "Assembling input file: '%s'", runtime.input_files->items[i]);
			indent_log();
			FILE *f;
			if (strcasecmp(runtime.input_files->items[i], "-") == 0) {
				f = stdin;
			} else {
				f = fopen(runtime.input_files->items[i], "r");
			}
			if (!f) {
				scas_abort("Unable to open '%s' for assembly.", runtime.input_files->items[i]);
			}
			assembler_settings_t settings = {
				.include_path = include_path,
				.set = instruction_set,
				.errors = errors,
				.warnings = warnings,
			};
			object_t *o = assemble(f, runtime.input_files->items[i], &settings);
			fclose(f);
			list_add(objects, o);
			deindent_log();
			scas_log(L_INFO, "Assembler returned %d errors, %d warnings for '%s'",
					errors->length, warnings->length, runtime.input_files->items[i]);
		}
	} else {
		int i;
		for (i = 0; i < runtime.input_files->length; ++i) {
			FILE *f;
			if (strcasecmp(runtime.input_files->items[i], "-") == 0) {
				f = stdin;
			} else {
				f = fopen(runtime.input_files->items[i], "r");
			}
			if (!f) {
				scas_abort("Unable to open '%s' for linking.", runtime.input_files->items[i]);
			}
			scas_log(L_INFO, "Loading object from file '%s'", runtime.input_files->items[i]);
			list_add(objects, freadobj(f, runtime.input_files->items[i]));
			/* TODO: Check for incompatible architectures */
			fclose(f);
		}
	}

	scas_log(L_DEBUG, "Opening output file for writing: %s", runtime.output_file);
	FILE *out;
	if (strcasecmp(runtime.output_file, "-") == 0) {
		out = stdout;
	} else {
		out = fopen(runtime.output_file, "w+");
	}
	if (!out) {
		scas_abort("Unable to open '%s' for output.", runtime.output_file);
	}

	if ((runtime.jobs & LINK) == LINK) {
		/* TODO: Linker scripts */
		scas_log(L_INFO, "Passing objects to linker");
		linker_settings_t settings = {
			.automatic_relocation = runtime.automatic_relocation,
			.merge_only = runtime.merge_only,
			.errors = errors,
			.warnings = warnings,
		};
		link_objects(out, objects, &settings);
		scas_log(L_INFO, "Linker returned %d errors, %d warnings", errors->length, warnings->length);
	} else {
		/* TODO: Link all provided assembly files together, or disallow mulitple input files when assembling */
		scas_log(L_INFO, "Skipping linking - writing to object file");
		object_t *o = objects->items[0];
		fwriteobj(out, o);
		fclose(out);
	}
	if (errors->length != 0) {
		int i;
		for (i = 0; i < errors->length; ++i) {
			error_t *error = errors->items[i];
			fprintf(stderr, "%s:%d:%d: error #%d: %s\n", error->file_name,
					(int)error->line_number, (int)error->column, error->code,
					get_error_string(error));
			fprintf(stderr, "%s\n", error->line);
			if (error->column != 0) {
				int j;
				for (j = error->column; j > 0; --j) {
					fprintf(stderr, ".");
				}
				fprintf(stderr, "^\n");
			} else {
				fprintf(stderr, "\n");
			}
		}
		remove(runtime.output_file);
	}
	if (warnings->length != 0) {
		int i;
		for (i = 0; i < errors->length; ++i) {
			warning_t *warning = warnings->items[i];
			fprintf(stderr, "%s:%d:%d: warning #%d: %s\n", warning->file_name,
					(int)warning->line_number, (int)warning->column, warning->code,
					get_warning_string(warning));
			fprintf(stderr, "%s\n", warning->line);
			if (warning->column != 0) {
				int j;
				for (j = warning->column; j > 0; --j) {
					fprintf(stderr, ".");
				}
				fprintf(stderr, "^\n");
			}
		}
	}

	int ret = errors->length;
	scas_log(L_DEBUG, "Exiting with status code %d, cleaning up", ret);
	list_free(runtime.input_files);
	list_free(include_path);
	list_free(objects);
	list_free(errors);
	list_free(warnings);
	instruction_set_free(instruction_set);
	return ret;
}
