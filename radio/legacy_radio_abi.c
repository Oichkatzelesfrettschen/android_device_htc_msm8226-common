#include <stddef.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>

/* HTC's old liblog returns the active mask and updates the caller's byte. */
int __htclog_init_mask(const char *tag, int index, uint8_t *mask)
{
    (void)tag;
    (void)index;
    if (mask == NULL) {
        return 0x3f;
    }
    *mask = 0x3f;
    return *mask;
}

/* Android 11 libc no longer exports the legacy I/O priority wrapper. */
int ioprio_set(int which, int who, int priority)
{
    return (int)syscall(SYS_ioprio_set, which, who, priority);
}
