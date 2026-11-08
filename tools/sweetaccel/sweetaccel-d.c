/*
 * sweetaccel-d: decides what the kernel's accelerator (SweetAccel.cpp) answers.
 *
 * The kernel hands this program every request a program makes of the accelerator (the render server
 * backboardd opens it and calls methods). Each one is logged to /var/tmp/sweetaccel.log with what it
 * carried, and answered by the table below; whatever is not in the table is answered "success" with an
 * all-zero result of the size the program asked for, which shows what it asks next.
 * Output: stdout and /var/tmp/sweetaccel.log.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>

#include "sweetaccel_proto.h"

typedef mach_port_t io_object_t, io_service_t, io_connect_t;
extern const mach_port_t kIOMasterPortDefault;
extern void *IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t, void *matching);
extern kern_return_t IOServiceOpen(io_service_t, task_t, uint32_t type, io_connect_t *);
extern kern_return_t IOObjectRelease(io_object_t);
extern kern_return_t IOConnectCallStructMethod(io_connect_t, uint32_t selector, const void *in, size_t inSize,
    void *out, size_t *outSize);

#define KR_TIMEOUT 0xe00002d6u

static FILE *logf_;
static unsigned lines;

static void
say(const char *fmt, ...)
{
	char line[400];
	va_list ap;

	if (lines > 3000) {
		return;
	}
	lines++;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fputs(line, stdout);
	fflush(stdout);
	if (logf_) {
		fputs(line, logf_);
		fflush(logf_);
	}
}

/* What is known about the program's requests: (type, selector) -> the answer. Add to it as it is learned. */
struct rule {
	uint32_t type, selector;
	int32_t result;
	uint32_t outsize;
	const uint8_t *out;
};
static const struct rule rules[] = {
	{ 0, 0, 0, 0, NULL },           /* placeholder: nothing is known yet */
};

static void
answer(const struct accel_ctrl_req *req, struct accel_ctrl_reply *rep)
{
	memset(rep, 0, sizeof(*rep));
	rep->slot = req->slot;
	rep->result = 0;
	if (req->selector < 0xff00) {
		rep->nsout = req->pad > ACC_SCALARS ? ACC_SCALARS : req->pad;      /* zeros */
		rep->outsize = req->outwanted > ACC_MAXIN ? ACC_MAXIN : req->outwanted;
	}
	for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
		if (rules[i].type == req->type && rules[i].selector == req->selector && rules[i].out != NULL) {
			rep->result = rules[i].result;
			rep->outsize = rules[i].outsize;
			memcpy(rep->out, rules[i].out, rules[i].outsize);
		}
	}
}

int
main(void)
{
	io_service_t service = 0;
	io_connect_t conn;
	static struct accel_ctrl_req req;
	static struct accel_ctrl_reply rep;
	char hex[200];

	logf_ = fopen("/var/tmp/sweetaccel.log", "a");
	say("sweetaccel-d: start, pid %d\n", getpid());
	for (int i = 0; i < 150 && service == 0; i++) {
		service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("SweetAccelControl"));
		if (service == 0) {
			sleep(2);
		}
	}
	if (service == 0 || IOServiceOpen(service, mach_task_self(), 0, &conn) != 0) {
		say("sweetaccel-d: cannot open the control service\n");
		return 1;
	}
	IOObjectRelease(service);
	say("sweetaccel-d: connected\n");
	for (;;) {
		size_t size = sizeof(req);
		kern_return_t kr = IOConnectCallStructMethod(conn, 0, NULL, 0, &req, &size);

		if (kr == KR_TIMEOUT) {
			continue;
		}
		if (kr != 0) {
			say("sweetaccel-d: next failed %#x\n", kr);
			sleep(1);
			continue;
		}
		hex[0] = 0;
		for (uint32_t i = 0; i < req.insize && i < 48; i++) {
			snprintf(hex + strlen(hex), sizeof(hex) - strlen(hex), "%02x", req.in[i]);
		}
		say("REQ %s %u sel %#x pid %u sin %u [%llx %llx %llx %llx] in %u out %u scalars-out %u %s\n",
		    req.selector >= 0xff00 ? "event" : "method", req.type, req.selector, req.pid, req.nsin,
		    (unsigned long long)req.sin[0], (unsigned long long)req.sin[1], (unsigned long long)req.sin[2],
		    (unsigned long long)req.sin[3], req.insize, req.outwanted, req.pad, hex);
		answer(&req, &rep);
		kr = IOConnectCallStructMethod(conn, 1, &rep, sizeof(rep), NULL, NULL);
		if (kr != 0) {
			say("sweetaccel-d: reply failed %#x\n", kr);
		}
	}
}
