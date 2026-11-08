/* qlogin - terminal login greeter, PAM + qlogind/logind CreateSession. Logic in mods/greeter.h; see readme.md and qlogin.1.
 * Build: cc -std=gnu11 -O2 -Isrc/mods src/qlogin.c -lpam -o bin/qlogin
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define QDBUS_IMPLEMENTATION
#include "mods/qdbus.h"
#define QLOGIN_IMPLEMENTATION
#include "mods/greeter.h"

int main(int argc, char **argv) { return qlogin_main(argc, argv); }
