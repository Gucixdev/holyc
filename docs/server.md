# vserv: the store over a socket

`src/vserv.c` serves one store file: Linux epoll, one worker thread per core, a fixed request buffer per connection
(allocated on its first byte, reused after), replies built in a per-thread buffer - a request allocates nothing.
The store is opened with `store_load` (mmap), so a 100M-row file is ready in milliseconds.

```sh
./run                                    # builds .build/vserv (BearSSL into .build/libbearssl.a first)
.build/vserv -s /run/vx.sock -p 8077 -t 2 -x docs.vxi            # Unix socket (0600) + TCP on 127.0.0.1
.build/vserv -p 8077 -L -r 0.95 -x docs.vxi                      # live: adds indexed as they come, no /build
.build/vserv -p 8443 -c cert.pem -k key.pem [-a clients-ca.pem] [-T] docs.vxi   # TLS, optional mutual TLS
curl -s localhost:8077/stats
curl -s -X POST 'localhost:8077/search?k=5' -d '[0.1, 0.2, ...]'
```

Every socket speaks two protocols, told apart by the first byte: HTTP/1.1 (`GET /health`, `GET /stats`,
`POST /search?k=`, `/add?id=`, `/delete?id=`, `/build`, `/save`; vectors as a JSON array or raw little-endian
f32, keep-alive, pipelining) and a binary protocol (`0xB5`, op, k, length; search/add/delete/build/save/stats)
with nothing to parse. The HTTP header scan looks for the blank line 16 bytes at a time with SSE2 (the
picohttpparser idea, cut down to what this server needs).

TLS (`-c`/`-k`): TLS 1.2 through the vendored [BearSSL](https://bearssl.org) 0.6 (MIT, unmodified, no crypto
code of our own): ECDHE only (forward secrecy), AEAD suites only (AES-GCM with AES-NI, ChaCha20-Poly1305), EC or
RSA keys, sessions resumed from one cache shared by every worker, keys wiped when a connection closes. `-a CA`
requires client certificates (mutual TLS). TLS is sniffed per connection (`0x16`): the TCP port takes only TLS
(`-P` lets plaintext in too); the Unix socket takes TLS or, for local tools, plaintext (`-T`: TLS only).

Measured: 2.4 MB idle RSS; 4.2 KB per open plaintext connection, 20.7 KB per TLS connection (300 open). From
Python clients on 2 cores: ~15-19k searches/s plaintext, ~10.7k over TLS on the first 2-core host; 8.9-10.6k and
5.7-7.5k on the Cascade Lake host of [benchmarks.md](benchmarks.md) (2k rows; the Python clients are the bottleneck).
`-L` puts the store in live mode (added rows are indexed by the server's own thread and the index is rebuilt in the
background as it ages - no `/build` calls) and `-r 0.95` keeps recall@10 at 0.95 (the probes tuned on the queries it
answers); see "Live ingest" in [vecdb.md](vecdb.md). `py/vserv_test.py` (29 checks: both protocols, both sockets, pipelining, a
request trickled byte by byte, oversized and malformed input, 8 concurrent clients, SIGTERM saves, restart, live mode:
3000 adds over the socket and no build call, indexed by the server in 0.19 s) and `py/vserv_tls_test.py` (22
checks: resumption on every reconnect, multi-record replies, plaintext refused, untrusted CA, mTLS accept/reject,
RSA key, curl, TLS on the Unix socket) run in `./run`.

