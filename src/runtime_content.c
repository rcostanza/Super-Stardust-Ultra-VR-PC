/* Offline AppContent provider for the configured base-game profile: no add-ons are installed;
 * no downloads, entitlement or license emulation. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define INVALID_ID ((int32_t)0x805a1000)
#define NOT_LOADED ((int32_t)0x805a1001)
#define PARAMETER ((int32_t)0x80d90002)
#define BUSY ((int32_t)0x80d90003)
#define NO_ENTITLEMENT ((int32_t)0x80d90007)
static unsigned refs, initialized, configured;
static uint32_t parameters[5];
static unsigned queries;
void runtime_content_configure(const uint32_t values[5]) {
    if (refs || initialized || (values[0]!=1 && values[0]!=3)) {
        fputs("ERROR: invalid or late content profile\n",stderr); exit(1);
    }
    memcpy(parameters,values,sizeof(parameters)); configured=1;
}
static void require_provider(void) {
    if (!configured) { fputs("STOP: AppContent needs --content-profile\n",stderr); exit(21); }
}
/* System modules: every library this eboot imports is provided by the host
 * runtime, so loading one only tracks a reference count. AppContent (0xb4)
 * additionally requires the configured content profile. */
static unsigned module_refs[0x10000];
/* sceSysmoduleLoadModule(id): Super Stardust Ultra VR ships no system module of its own to start (sce_module
 * holds only libc and Fios2, linked at boot). */
static ABI int32_t module_load(uint16_t id) {
    if (!id) return INVALID_ID;
    if (id==0xb4) require_provider();
    if (module_refs[id]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); exit(21); }
    ++module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; printf("Runtime: AppContent host provider loaded; references=%u\n",refs); }
    else printf("Runtime: system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
static ABI int32_t module_loaded(uint16_t id) {
    if (!id) return INVALID_ID;
    return module_refs[id] ? 0 : NOT_LOADED;
}
static ABI int32_t module_unload(uint16_t id) {
    int32_t result=module_loaded(id);
    if (result) return result;
    --module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; if (!refs) initialized=0; }
    return 0;
}
static void require_loaded(void) {
    require_provider();
    if (!refs) { fputs("STOP: AppContent called before module load\n",stderr); exit(21); }
}
static ABI int32_t content_init(const unsigned char *init,unsigned char *boot) {
    require_loaded();
    if (!init || !boot) return PARAMETER;
    if (initialized) return BUSY;
    /* Only the reserved-zero init profile is implemented. */
    for (unsigned i=0;i<32;++i) if (init[i]) return PARAMETER;
    memset(boot,0,40); initialized=1;
    puts("Runtime: AppContent initialized; offline base-game profile");
    return 0;
}
static void require_initialized(void) {
    require_loaded();
    if (!initialized) { fputs("STOP: AppContent query before initialization\n",stderr); exit(21); }
}
static ABI int32_t param_int(uint32_t id,int32_t *out) {
    require_initialized();
    if (id>4 || !out) return PARAMETER;
    memcpy(out,&parameters[id],4); ++queries;
    printf("Runtime: AppContent parameter %u = %d\n",id,*out);
    return 0;
}
/* sceAppContentGetAddcontInfo(service, label, info): DRM_NO_ENTITLEMENT for every label, as on a
 * PS4 with no add-ons installed. Super Stardust Ultra VR asks for each of its ships and skins this way. */
static ABI int32_t addon_info(uint32_t service,const char *label,void *info) {
    require_initialized();
    if (service) { fputs("STOP: nonzero AppContent service label unsupported\n",stderr); exit(21); }
    if (!label || !info) return PARAMETER;
    ++queries;
    return NO_ENTITLEMENT;
}
/* sceAppContentTemporaryDataMount2(option, mount) (shadPS4 app_content.cpp): names /temp0, kept in
 * <user>/temp0; the folder is made and mounted on the first call (Super Stardust Ultra VR's boot makes none). */
static ABI int32_t temp_mount(uint32_t option,char *mount) {
    static int mounted;
    (void)option;
    if (!mount) return PARAMETER;
    if (!mounted) {
        char path[600]; snprintf(path,sizeof(path),"%s/temp0",runtime_file_user_dir());
        mkdir(path,0755);
        if (runtime_file_mount("/temp0",path)) return PARAMETER;
        mounted=1;
    }
    memset(mount,0,16); memcpy(mount,"/temp0",6); return 0;
}
uintptr_t runtime_content_resolve(const char *name) {
    if (!strcmp(name,"buYbeLOGWmA#libSceAppContent")) return (uintptr_t)temp_mount;
    if (!strcmp(name,"g8cM39EUZ6o#libSceSysmodule")) return (uintptr_t)module_load;
    if (!strcmp(name,"fMP5NHUOaMk#libSceSysmodule")) return (uintptr_t)module_loaded;
    if (!strcmp(name,"eR2bZFAAU0Q#libSceSysmodule")) return (uintptr_t)module_unload;
    if (!strcmp(name,"R9lA82OraNs#libSceAppContent")) return (uintptr_t)content_init;
    if (!strcmp(name,"99b82IKXpH4#libSceAppContent")) return (uintptr_t)param_int;
    if (!strcmp(name,"m47juOmH0VE#libSceAppContent")) return (uintptr_t)addon_info;
    return 0;
}
void runtime_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, queries=%u\n",refs,initialized,queries);
}
