/* AppContent: no add-ons are installed, so every label the game asks for
 * (sceAppContentGetAddcontInfo) has no entitlement. /temp0 is made and mounted on the first
 * sceAppContentTemporaryDataMount2, not at start. */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_content.c"
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>

static char user_dir[256];
const char *runtime_file_user_dir(void) { return user_dir; }
static char mounted_guest[32], mounted_host[700];
static int mount_calls;
int runtime_file_mount(const char *guest,const char *host) {
    snprintf(mounted_guest,sizeof(mounted_guest),"%s",guest); snprintf(mounted_host,sizeof(mounted_host),"%s",host);
    ++mount_calls; return 0;
}

int main(void) {
    const char *tmp=getenv("TMP");
    if (!tmp) tmp=getenv("TEMP");
    if (!tmp) tmp="/tmp";
    char path[700];
    snprintf(user_dir,sizeof(user_dir),"%s/stardust-content-test-%d",tmp,(int)getpid());
    mkdir(user_dir,0755);

    uint32_t profile[5]={1,0,0,0,0};
    unsigned char init[32]={0}, boot[40], info[24];
    runtime_content_configure(profile);
    assert(!module_load(0xb4));
    assert(!content_init(init,boot));

    /* Every add-on label: no entitlement; bad arguments: PARAMETER. */
    char label[17]="ANLASERGUN000001";
    assert(addon_info(0,label,info)==NO_ENTITLEMENT);
    assert(addon_info(0,NULL,info)==PARAMETER && addon_info(0,label,NULL)==PARAMETER);

    /* /temp0: nothing until the game asks; then <user>/temp0, mounted once. */
    struct stat st;
    snprintf(path,sizeof(path),"%s/temp0",user_dir);
    assert(stat(path,&st) && !mount_calls);
    char mount[16];
    assert(temp_mount(0,NULL)==PARAMETER);
    assert(!temp_mount(0,mount) && !strcmp(mount,"/temp0"));
    assert(!temp_mount(0,mount) && mount_calls==1);
    assert(!stat(path,&st) && !strcmp(mounted_guest,"/temp0") && !strcmp(mounted_host,path));

    rmdir(path); rmdir(user_dir);
    puts("content test: ok");
    return 0;
}
