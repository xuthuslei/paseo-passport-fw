/* Host fixture: decrypt a real daemon frame using the FIRMWARE COMPONENT
 * SOURCES verbatim (not copies):
 *   components/paseo_client/paseo_box.c   (derive + decrypt)
 *   components/paseo_client/pbase64.c     (frame base64 decode)
 *   components/tweetnacl/tweetnacl.c      (crypto primitives)
 * Pipeline mirrors paseo_client.c decrypt_frame() exactly:
 *   rlen = pb64_decode(frame_b64, raw, 48*1024)
 *   paseo_box_derive(daemon_pub, dev_sec, shared)
 *   plen = paseo_box_decrypt(shared, raw, rlen, plain, 48*1024)
 * Inputs come from ref_probe.py's probe_out.json. The derived shared key
 * and the decrypted plaintext are written to binary files so the caller
 * can compare them (byte-for-byte / by hash) without printing secrets.
 *
 * Usage: host_fw_decrypt <probe_out.json> <shared_out.bin> <plain_out.bin>
 * Exit codes: 0 decrypt ok; 2 usage/io; 3 derive failed; 4 decrypt failed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "paseo_box.h"
#include "pbase64.h"

#define MAX_FRAME_BYTES (48 * 1024) /* same as paseo_client.c */

static char *read_file(const char *path, long *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    if (n && fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    buf[n] = '\0';
    *len_out = n;
    return buf;
}

/* Extract a simple JSON string field value ("key":"value", no escapes in
 * base64 values) into a malloc'd buffer. Returns NULL if not found. */
static char *json_str_dup(const char *json, const char *key)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    if (*p != '"') return NULL;
    p++;
    const char *e = strchr(p, '"');
    if (!e) return NULL;
    size_t n = (size_t)(e - p);
    char *out = malloc(n + 1);
    memcpy(out, p, n);
    out[n] = '\0';
    return out;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <probe_out.json> <shared_out> <plain_out>\n", argv[0]);
        return 2;
    }
    long jlen;
    char *json = read_file(argv[1], &jlen);
    if (!json) { fprintf(stderr, "cannot read %s\n", argv[0]); return 2; }
    char *dev_b64 = json_str_dup(json, "dev_priv_b64");
    char *pub_b64 = json_str_dup(json, "daemon_pub_b64");
    char *frame_b64 = json_str_dup(json, "frame_b64");
    if (!dev_b64 || !pub_b64 || !frame_b64) {
        fprintf(stderr, "missing fields in probe_out.json\n");
        return 2;
    }
    printf("input: dev_priv_b64 %zu chars, daemon_pub_b64 %zu chars, frame_b64 %zu chars\n",
           strlen(dev_b64), strlen(pub_b64), strlen(frame_b64));

    uint8_t dev_sec[32], daemon_pub[32];
    int sl = pb64_decode(dev_b64, dev_sec, sizeof(dev_sec));
    int pl = pb64_decode(pub_b64, daemon_pub, sizeof(daemon_pub));
    if (sl != 32 || pl != 32) {
        fprintf(stderr, "key decode failed: sec %d pub %d\n", sl, pl);
        return 2;
    }

    uint8_t shared[32];
    if (!paseo_box_derive(daemon_pub, dev_sec, shared)) {
        fprintf(stderr, "paseo_box_derive failed (zero scalarmult)\n");
        return 3;
    }
    FILE *sf = fopen(argv[2], "wb");
    if (sf) { fwrite(shared, 1, 32, sf); fclose(sf); }

    /* decrypt_frame() replica (paseo_client.c:867-882) */
    int cap = MAX_FRAME_BYTES;
    uint8_t *raw = malloc(cap);
    uint8_t *plain = malloc(MAX_FRAME_BYTES + 1);
    int rlen = pb64_decode(frame_b64, raw, cap);
    printf("pb64_decode(frame) -> rlen %d\n", rlen);
    if (rlen < 0) { fprintf(stderr, "pb64_decode failed\n"); return 4; }
    int plen = paseo_box_decrypt(shared, raw, rlen, plain, MAX_FRAME_BYTES);
    printf("paseo_box_decrypt -> plen %d\n", plen);
    if (plen < 0) {
        fprintf(stderr, "DECRYPT FAILED (component pipeline)\n");
        return 4;
    }
    FILE *pf = fopen(argv[3], "wb");
    if (pf) { fwrite(plain, 1, plen, pf); fclose(pf); }
    printf("DECRYPT OK: %d plaintext bytes written\n", plen);
    return 0;
}
