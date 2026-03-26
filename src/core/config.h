/*
 * config.h - Configuration parsing
 */

#ifndef __CONFIG_H
#define __CONFIG_H

#include <stdio.h>

/* Configuration file (set by command line argument, default: config.json) */
extern char *cfg_file;
extern char *log_file;

/* Initialize logging */
void init_log(const char *log_file);

/* Parse configuration file */
void read_config(const char *cfg_file);

/* Parse command line arguments */
void parse_args(int argc, char *argv[]);

/* Global log level */
extern int loglevel;

/* Check if should log at given level */
bool should_log(int level);

/* Global log file pointer */
extern FILE *log_fp;

/* Auto mode flag (defined in config.cpp) */
extern int auto_mode;

/* UI mode flag (0 = no UI, 1 = use NCursesUI) */
extern int use_ui;

/* Batch mode: command script file */
extern char *batch_file;

/* Cleanup allocated configuration memory */
void cleanup_config();

#endif /* __CONFIG_H */
