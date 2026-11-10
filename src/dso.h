#ifndef DSO_H
#define DSO_H
#include "macho.h"
struct dso;
struct dso *dso_load_main(const char *path);
int  dso_bind_all(int verbose);
void dso_run_init_funcs(void);
int  dso_count(void);
void dso_list(void);
image_t *dso_main_image(void);
const char *dso_path_at(int i);
image_t *dso_image_at(int i);
#endif
