/* ArkOS account service. Original implementation, MIT license. */
#ifndef ARK_ACCOUNTS_H
#define ARK_ACCOUNTS_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ACCOUNTS_MAX 8u
#define ACCOUNTS_UID_NONE UINT32_MAX
#define ACCOUNTS_FIRST_UID 1000u
#define ACCOUNTS_PASSWORD_MIN 8u
#define ACCOUNTS_PASSWORD_MAX 64u
#define ACCOUNTS_KDF_ITERATIONS 60000u
#define ACCOUNTS_DB_PATH "/.system/accounts"
#define ACCOUNT_ADMIN 1u
#define ACCOUNT_DISABLED 2u

typedef enum {
    ACCOUNT_NEEDS_SETUP = 0,
    ACCOUNT_LOGGED_OUT = 1,
    ACCOUNT_LOCKED = 2,
    ACCOUNT_ACTIVE = 3,
    ACCOUNT_ERROR = 4
} AccountState;

/* Public data only. Never put salts or verifiers into an IPC response. */
typedef struct {
    uint32_t uid, flags;
    char slug[24];
    char display_name[64];
    char home[64];
} AccountProfile;

/* Required adapter supplied by the kernel syscall dispatcher. It must inspect
 * the actual current process, never a UID/capability in an untrusted request.
 * Kernel boot initialization is trusted. No default-allow implementation. */
bool accounts_caller_is_system(void);

bool accounts_init(void); /* after vfs_init; existing malformed DB fails closed */
AccountState accounts_state(void);
const char *accounts_error(void);
bool accounts_persistent(void);
unsigned accounts_count(void);
bool accounts_list(unsigned index, AccountProfile *out);
bool accounts_profile(uint32_t uid, AccountProfile *out);
bool accounts_session_profile(AccountProfile *out); /* active or locked */
uint32_t accounts_current_uid(void);                /* NONE unless ACTIVE */
const char *accounts_current_home(void);            /* empty unless ACTIVE */
bool accounts_current_admin(void);
uint64_t accounts_session_generation(void);
uint32_t accounts_retry_after_ticks(void); /* PIT=100 Hz; shared failure limit */

/* All functions below require a verified SYSTEM caller. Enrollment creates
 * Ark / ark / UID 1000 / /home/ark, with the supplied password and no default.
 * Login only starts a logged-out session; switching first requires logout.
 * Lock keeps a public selected profile; unlock authenticates only that user. */
bool accounts_enroll(const char *password);
bool accounts_login(const char *slug, const char *password);
bool accounts_lock(void);
bool accounts_unlock(const char *password);
bool accounts_logout(void);
bool accounts_change_password(const char *old_password, const char *new_password);
bool accounts_create_user(const char *slug, const char *display_name, const char *password,
                          bool administrator);
bool accounts_set_disabled(uint32_t uid, bool disabled);

/* Canonical absolute path gate for application file services. Invalid paths,
 * /.system and other users' homes are rejected, including for admin sessions.
 * Contents of own home are readable/writable; the home root cannot be removed
 * or renamed. /etc and /mnt are readable; /mnt also writable
 * (the external backend enforces read-only mounts). Directory ancestors / and
 * /home are listable only: the dispatcher must filter every returned child.
 * Any relative path must first be resolved by the caller under the user's home. */
bool accounts_path_allowed(uint32_t uid, const char *absolute_path, bool write);

#endif
