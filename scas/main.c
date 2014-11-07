#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "log.h"
#include "list.h"
#include "enums.h"
#include "errors.h"
#include "assembler.h"
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
	runtime.linker_script = NULL;
	runtime.verbosity = L_WARNING;
}

void validate_runtime() {
	if (runtime.input_files->length == 0) {
		scas_abort("No input files given");
	}
	if (runtime.output_file == NULL) {
		/* Auto-assign an output file name */
		const char *bin = ".bin";
		runtime.output_file = malloc(strlen(runtime.input_files->items[0]) + sizeof(bin));
		memcpy(runtime.output_file, runtime.input_files->items[0], strlen(runtime.input_files->items[0]));
		int i = strlen(runtime.output_file);
		while (runtime.output_file[--i] != '.' && i != 0);
		if (i == 0) {
			i = strlen(runtime.output_file);
		}
		int j;
		for (j = 0; j < sizeof(bin); j++) {
			runtime.output_file[i + j] = bin[j];
		}
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

int main(int argc, char **argv) {
	init_runtime();
	parse_arguments(argc, argv);
	init_log(runtime.verbosity);
	validate_runtime();
	instruction_set_t *instruction_set = find_inst();
	list_t *errors = create_list();
	list_t *warnings = create_list();

	list_t *objects = create_list();
	if ((runtime.jobs & ASSEMBLE) == ASSEMBLE) {
		int i;
		for (i = 0; i < runtime.input_files->length; ++i) {
			FILE *f;
			if (strcasecmp(runtime.input_files->items[i], "-") == 0) {
				f = stdin;
			} else {
				f = fopen(runtime.input_files->items[i], "r");
			}
			if (!f) {
				scas_abort("Unable to open '%s' for assembly.", runtime.input_files->items[i]);
			}
			object_t *o = assemble(f, runtime.input_files->items[i], instruction_set, errors, warnings);
			fclose(f);
			list_add(objects, o);
			/* Temporary test code */
			int ai;
			for (ai = 0; ai < o->areas->length; ++ai) {
				area_t *area = o->areas->items[ai];
				fprintf(stderr, "Area '%s':\nMachine code:\n", area->name);
				int j;
				for (j = 0; j < area->data_length; j += 16) {
					fprintf(stderr, "\t");
					int k;
					for (k = 0; k < 16 && j + k < area->data_length; ++k) {
						fprintf(stderr, "%02X ", area->data[j + k]);
					}
					fprintf(stderr, "\n");
				}
				if (area->late_immediates->length != 0) {
					fprintf(stderr, "Unresolved immediate values:\n");
					for (j = 0; j < area->late_immediates->length; ++j) {
						late_immediate_t *imm = area->late_immediates->items[j];
						fprintf(stderr, "\t0x%04X: '", (uint16_t)imm->address);
						print_tokenized_expression(stderr, imm->expression);
						fprintf(stderr, "' (width: %d)\n", (int)imm->width);
					}
				}
				if (area->symbols->length != 0) {
					fprintf(stderr, "Symbols:\n");
					for (j = 0; j < area->symbols->length; ++j) {
						symbol_t *sym = area->symbols->items[j];
						printf("\t%s: 0x%04X\n", sym->name, (unsigned int)sym->value);
					}
				}
			}
		}
	} else {
		/* TODO: Load object files from disk */
	}
	if ((runtime.jobs & LINK) == LINK) {
		/* TODO: Link objects */
	} else {
		FILE *f;
		if (strcasecmp(runtime.output_file, "-") == 0) {
			f = stdout;
		} else {
			f = fopen(runtime.output_file, "w+");
		}
		if (!f) {
			scas_abort("Unable to open '%s' for output.", runtime.output_file);
		}
		object_t *o = objects->items[0];
		fwriteobj(f, o, runtime.arch);
		fclose(f);
	}
	if (errors->length != 0) {
		int i;
		for (i = 0; i < errors->length; ++i) {
			error_t *error = errors->items[i];
			fprintf(stderr, "%s:%d:%d: error #%d: %s\n", error->file_name,
					(int)error->line_number, (int)error->column, error->code,
					get_error_string(error));
			fprintf(stderr, "%s\n", error->line);
			int j;
			for (j = error->column; j > 0; --j) {
				fprintf(stderr, ".");
			}
			fprintf(stderr, "^\n");
		}
		list_free(runtime.input_files);
		list_free(objects);
		list_free(errors);
		list_free(warnings);
		instruction_set_free(instruction_set);
		return errors->length;
	}

	list_free(runtime.input_files);
	list_free(objects);
	list_free(errors);
	list_free(warnings);
	instruction_set_free(instruction_set);
	return 0;
}
