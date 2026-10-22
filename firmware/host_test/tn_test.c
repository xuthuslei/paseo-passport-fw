/* Host cross-check of the exact box wrapper the firmware will use.
 * Modes:
 *   tn_test decrypt <dev_sec> <daemon_pub> <token_file> <out_file>
 *   tn_test encrypt <dev_sec> <daemon_pub> <plain_file> <token_out>
 * token format = nonce(24) || ciphertext(msg+16), same as Paseo wire format
 * after base64 decoding. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tweetnacl.h"

/* host randombytes shim (device uses esp_fill_random) */
#include <fcntl.h>
#include <unistd.h>
void randombytes(unsigned char *x, unsigned long long xlen)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) { perror("urandom"); exit(2); }
    unsigned long long off = 0;
    while (off < xlen) {
        ssize_t r = read(fd, x + off, (size_t)(xlen - off));
        if (r <= 0) { perror("read"); exit(2); }
        off += (unsigned long long)r;
    }
    close(fd);
}

static unsigned char *read_file(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(n ? n : 1);
    if (n && fread(buf, 1, n, f) != (size_t)n) { perror("fread"); exit(2); }
    fclose(f); *len = n; return buf;
}
static void write_file(const char *path, const unsigned char *buf, long len)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    if (len && fwrite(buf, 1, len, f) != (size_t)len) { perror("fwrite"); exit(2); }
    fclose(f);
}

/* --- wrappers identical to firmware paseo_box.c logic --- */
static int box_encrypt(const unsigned char shared[32], const unsigned char *in,
                       unsigned long inlen, unsigned char *out /* nonce||ct */)
{
    unsigned char nonce[24];
    randombytes(nonce, 24);
    unsigned long mlen = inlen + 32;
    unsigned char *m = calloc(mlen, 1), *c = malloc(mlen);
    memcpy(m + 32, in, inlen);
    if (crypto_box_afternm(c, m, mlen, nonce, shared) != 0) return -1;
    memcpy(out, nonce, 24);
    memcpy(out + 24, c + 16, inlen + 16);
    free(m); free(c);
    return 0;
}
static int box_decrypt(const unsigned char shared[32], const unsigned char *in,
                       unsigned long inlen, unsigned char *out, unsigned long *outlen)
{
    if (inlen < 40) return -1;
    unsigned long ctlen = inlen - 24;          /* ciphertext incl. 16B MAC */
    unsigned long clen = ctlen + 16;           /* padded length */
    unsigned char *c = calloc(clen, 1), *m = malloc(clen);
    memcpy(c + 16, in + 24, ctlen);
    if (crypto_box_open_afternm(m, c, clen, in, shared) != 0) return -1;
    *outlen = clen - 32;
    memcpy(out, m + 32, *outlen);
    free(c); free(m);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 6) { fprintf(stderr, "usage\n"); return 2; }
    long sl, pl, il;
    unsigned char *sec = read_file(argv[2], &sl);
    unsigned char *pub = read_file(argv[3], &pl);
    unsigned char *in = read_file(argv[4], &il);
    if (sl != 32 || pl != 32) { fprintf(stderr, "bad key len\n"); return 2; }
    /* zero check like paseo crypto.ts deriveSharedKey */
    unsigned char raw[32];
    crypto_scalarmult(raw, sec, pub);
    int allzero = 1; for (int i = 0; i < 32; i++) if (raw[i]) allzero = 0;
    if (allzero) { fprintf(stderr, "zero shared secret\n"); return 1; }
    unsigned char shared[32];
    crypto_box_beforenm(shared, pub, sec);
    if (!strcmp(argv[1], "decrypt")) {
        unsigned char *out = malloc(il);
        unsigned long outlen = 0;
        if (box_decrypt(shared, in, il, out, &outlen) != 0) {
            fprintf(stderr, "decrypt FAILED\n"); return 1;
        }
        write_file(argv[5], out, (long)outlen);
    } else if (!strcmp(argv[1], "encrypt")) {
        unsigned char *out = malloc(il + 40);
        if (box_encrypt(shared, in, il, out) != 0) {
            fprintf(stderr, "encrypt FAILED\n"); return 1;
        }
        write_file(argv[5], out, il + 40);
    } else { fprintf(stderr, "bad mode\n"); return 2; }
    return 0;
}
