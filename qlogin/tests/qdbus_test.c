/* qdbus_test: marshalling round trips, fd passing over a socketpair, and Hello + a call on a private bus when QD_TEST_BUS is set. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <sys/socket.h>
#define QDBUS_IMPLEMENTATION
#include "../src/mods/qdbus.h"

static int fails;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); fails++; } } while (0)

int main(void)
{
	qd_buf b = {0};
	qd_rd r;
	const char *sg;
	/* alignment + arrays + variants + dict entries */
	qd_put_u8(&b, 7);
	qd_put_str(&b, "hello");
	qd_arr a = qd_arr_begin(&b, 8);
	for (int i = 0; i < 3; i++) { qd_align(&b, 8); qd_put_str(&b, "k"); qd_variant_u64(&b, 100 + i); }
	qd_arr_end(&b, a);
	qd_put_u32(&b, 0xdeadbeef);
	r = (qd_rd){ b.b, 0, b.n, 0 };
	CHECK(qd_r_u8(&r) == 7);
	CHECK(!strcmp(qd_r_str(&r), "hello"));
	size_t end = qd_r_array(&r, 8);
	int n = 0;
	while (r.pos < end) { r.pos = (r.pos + 7) & ~7u; CHECK(!strcmp(qd_r_str(&r), "k")); sg = qd_r_sig(&r); CHECK(*sg == 't'); CHECK(qd_r_u64(&r) == 100u + n); n++; }
	CHECK(n == 3);
	CHECK(qd_r_u32(&r) == 0xdeadbeef);
	CHECK(!r.bad && r.pos == b.n);
	/* skip over a complex signature */
	r = (qd_rd){ b.b, 1, b.n, 0 };
	sg = "sa{sv}u";
	qd_r_skip(&r, &sg); qd_r_skip(&r, &sg); qd_r_skip(&r, &sg);
	CHECK(!r.bad && r.pos == b.n && !*sg);
	CHECK(qd_sig_len("a(susso)x") == 8);
	/* truncated input must flag bad, not crash */
	r = (qd_rd){ b.b, 0, 5, 0 };
	qd_r_u8(&r); qd_r_str(&r);
	CHECK(r.bad);
	free(b.b);

	/* a message with an fd over a socketpair */
	int sp[2], pp[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0);
	CHECK(pipe(pp) == 0);
	qd_conn tx = { .fd = sp[0], .serial = 1 }, rx = { .fd = sp[1], .serial = 1 };
	qd_buf body = {0};
	qd_put_str(&body, "payload");
	qd_put_u32(&body, 0);   /* index of the fd in a real 'h' */
	CHECK(qd_send_call(&tx, "x.y", "/a/b", "x.y.I", "Meth", "sh", &body, &pp[0], 1) != 0);
	qd_msg m;
	CHECK(qd_recv(&rx, &m, 1000) == 1);
	CHECK(m.type == 1 && !strcmp(m.path, "/a/b") && !strcmp(m.iface, "x.y.I") && !strcmp(m.member, "Meth") && !strcmp(m.sig, "sh") && !strcmp(m.dest, "x.y"));
	CHECK(m.nfds == 1);
	if (m.nfds == 1) {
		CHECK(write(pp[1], "z", 1) == 1);
		char ch = 0;
		CHECK(read(m.fds[0], &ch, 1) == 1 && ch == 'z');
	}
	qd_msg_free(&m);
	CHECK(qd_recv(&rx, &m, 50) == 0);   /* timeout */
	free(body.b);
	qd_close(&tx); qd_close(&rx); close(pp[0]); close(pp[1]);

	const char *bus = getenv("QD_TEST_BUS");
	if (bus) {
		qd_conn c;
		CHECK(qd_open(&c, bus) == 0);
		CHECK(c.name[0] == ':');
		qd_msg rp;
		qd_buf nb = {0};
		qd_put_str(&nb, "org.freedesktop.DBus");
		CHECK(qd_call(&c, &rp, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner", "s", &nb, NULL, 0, 2000) == 0);
		qd_rd q = { rp.body, 0, rp.blen, 0 };
		CHECK(qd_r_u32(&q) == 1);
		qd_msg_free(&rp);
		free(nb.b);
		/* an error reply is reported as 1 */
		nb = (qd_buf){0};
		CHECK(qd_call(&c, &rp, "org.nonexistent.Nope", "/", "a.b", "C", NULL, NULL, NULL, 0, 2000) == 1);
		CHECK(!strcmp(rp.err, "org.freedesktop.DBus.Error.ServiceUnknown"));
		qd_msg_free(&rp);
		qd_close(&c);
	}
	if (!fails) puts("qdbus_test: all ok");
	return fails != 0;
}
