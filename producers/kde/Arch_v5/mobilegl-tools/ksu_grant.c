/*
 * ksu_grant -- grant a KernelSU root profile to a package without the manager UI.
 *
 * KernelSU (3.0, supercall ABI, upstream 4d396fda) only accepts
 * GET/SET_APP_PROFILE from the manager UID (is_manager() == current_uid %
 * 100000 == manager appid). Root itself is refused. This helper runs as root,
 * drops to the manager UID, installs the [ksu_driver] fd through the reboot
 * magic hook, and sets an allow_su profile with the default root profile
 * (uid 0, full caps, u:r:su:s0) for the target package.
 *
 * Usage: ksu_grant <manager-uid> <package> <package-uid> [revoke]
 * Exit 0 on success. Idempotent.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#define KSU_INSTALL_MAGIC1 0xDEADBEEFu
#define KSU_INSTALL_MAGIC2 0xCAFEBABEu

#define KSU_APP_PROFILE_VER 2
#define KSU_MAX_PACKAGE_NAME 256
#define KSU_MAX_GROUPS 32
#define KSU_SELINUX_DOMAIN 64

struct root_profile {
    int32_t uid;
    int32_t gid;
    int32_t groups_count;
    int32_t groups[KSU_MAX_GROUPS];
    struct {
        uint64_t effective;
        uint64_t permitted;
        uint64_t inheritable;
    } capabilities;
    char selinux_domain[KSU_SELINUX_DOMAIN];
    int32_t namespaces;
};

struct non_root_profile {
    uint8_t umount_modules;
};

struct app_profile {
    uint32_t version;
    char key[KSU_MAX_PACKAGE_NAME];
    int32_t current_uid;
    uint8_t allow_su;
    union {
        struct {
            uint8_t use_default;
            char template_name[KSU_MAX_PACKAGE_NAME];
            struct root_profile profile;
        } rp_config;
        struct {
            uint8_t use_default;
            struct non_root_profile profile;
        } nrp_config;
    };
};

struct ksu_app_profile_cmd {
    struct app_profile profile;
};

/* _IOC(dir, 'K', nr, 0): dir READ=2 WRITE=1, Linux generic ioctl layout */
#define KSU_IOC(dir, nr) \
    ((uint32_t)(((dir) << 30) | ((uint32_t)'K' << 8) | (nr)))
#define KSU_IOCTL_GET_APP_PROFILE KSU_IOC(3, 11)
#define KSU_IOCTL_SET_APP_PROFILE KSU_IOC(1, 12)

static int install_driver_fd(void)
{
    int fd = -1;
    syscall(SYS_reboot, KSU_INSTALL_MAGIC1, KSU_INSTALL_MAGIC2, 0, &fd);
    return fd;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s <manager-uid> <package> <package-uid> [revoke]\n",
                argv[0]);
        return 64;
    }
    if (getuid() != 0) {
        fprintf(stderr, "must start as root\n");
        return 69;
    }
    long manager_uid = atol(argv[1]);
    const char *package = argv[2];
    long package_uid = atol(argv[3]);
    int revoke = argc > 4 && strcmp(argv[3 + 1], "revoke") == 0;
    if (strlen(package) >= KSU_MAX_PACKAGE_NAME) {
        fprintf(stderr, "package name too long\n");
        return 64;
    }

    if (setresgid(manager_uid, manager_uid, manager_uid) != 0 ||
        setresuid(manager_uid, manager_uid, manager_uid) != 0) {
        fprintf(stderr, "setresuid(%ld) failed: %s\n", manager_uid,
                strerror(errno));
        return 70;
    }

    int fd = install_driver_fd();
    if (fd < 0) {
        fprintf(stderr, "ksu driver fd install failed: %s\n", strerror(errno));
        return 71;
    }

    struct ksu_app_profile_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.profile.version = KSU_APP_PROFILE_VER;
    strcpy(cmd.profile.key, package);
    cmd.profile.current_uid = (int32_t)package_uid;

    if (ioctl(fd, KSU_IOCTL_GET_APP_PROFILE, &cmd) == 0) {
        printf("before: allow_su=%d uid=%d key=%s\n", cmd.profile.allow_su,
               cmd.profile.current_uid, cmd.profile.key);
    } else {
        printf("before: GET_APP_PROFILE: %s (entry may not exist yet)\n",
               strerror(errno));
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.profile.version = KSU_APP_PROFILE_VER;
    strcpy(cmd.profile.key, package);
    cmd.profile.current_uid = (int32_t)package_uid;
    cmd.profile.allow_su = revoke ? 0 : 1;
    if (!revoke) {
        /* Mirror the kernel default root profile; profile_valid() requires
         * a non-empty domain and groups_count <= KSU_MAX_GROUPS even when
         * use_default is set. */
        cmd.profile.rp_config.use_default = 1;
        cmd.profile.rp_config.profile.uid = 0;
        cmd.profile.rp_config.profile.gid = 0;
        cmd.profile.rp_config.profile.groups_count = 1;
        cmd.profile.rp_config.profile.groups[0] = 0;
        cmd.profile.rp_config.profile.capabilities.effective = UINT64_MAX;
        cmd.profile.rp_config.profile.capabilities.permitted = UINT64_MAX;
        cmd.profile.rp_config.profile.capabilities.inheritable = 0;
        strcpy(cmd.profile.rp_config.profile.selinux_domain, "u:r:su:s0");
        cmd.profile.rp_config.profile.namespaces = 0;
    } else {
        cmd.profile.nrp_config.use_default = 1;
        cmd.profile.nrp_config.profile.umount_modules = 1;
    }

    if (ioctl(fd, KSU_IOCTL_SET_APP_PROFILE, &cmd) != 0) {
        fprintf(stderr, "SET_APP_PROFILE failed: %s\n", strerror(errno));
        return 72;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.profile.version = KSU_APP_PROFILE_VER;
    strcpy(cmd.profile.key, package);
    cmd.profile.current_uid = (int32_t)package_uid;
    if (ioctl(fd, KSU_IOCTL_GET_APP_PROFILE, &cmd) != 0) {
        fprintf(stderr, "verify GET_APP_PROFILE failed: %s\n", strerror(errno));
        return 73;
    }
    printf("after: allow_su=%d uid=%d key=%s\n", cmd.profile.allow_su,
           cmd.profile.current_uid, cmd.profile.key);
    return cmd.profile.allow_su == (revoke ? 0 : 1) ? 0 : 74;
}
