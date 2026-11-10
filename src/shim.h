#ifndef SHIM_H
#define SHIM_H
void *dshim_lookup(const char *macho_name);
void  shim_set_args(int argc, char **argv, char **envp);
void  shim_notify_image_added(void *image);
#endif
