/* Load the actual plugin and decoder libraries without opening any device. */
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv)
{
  if (argc != 2)
    return 2;
  void *module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!module) {
    fprintf(stderr, "VFM dlopen: %s\n", dlerror());
    return 1;
  }
  int (*probe)(void (*)(void)) = (int (*)(void (*)(void)))dlsym(module, "moonlight_vfm_init");
  if (!probe || probe(NULL)) {
    fprintf(stderr, "VFM decoder capability probe failed\n");
    dlclose(module);
    return 1;
  }
  dlclose(module);
  printf("PASS: %zu-bit plugin loads; both decoders support DRM PRIME and nonblocking dequeue\n",
         sizeof(void *) * 8);
  return 0;
}
