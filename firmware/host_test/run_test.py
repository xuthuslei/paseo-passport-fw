import subprocess, sys
from nacl.public import PrivateKey, PublicKey, Box

D = "/tmp/tntest"; import os; os.makedirs(D, exist_ok=True)
BIN = sys.argv[1]
dev = PrivateKey.generate(); daemon = PrivateKey.generate()
open(f"{D}/dev_sec", "wb").write(bytes(dev))
open(f"{D}/daemon_pub", "wb").write(bytes(daemon.public_key))
open(f"{D}/daemon_sec", "wb").write(bytes(daemon))

msg = b'{"type":"session","message":{"type":"fetch_agents_request","requestId":"x"}}'
box = Box(dev, daemon.public_key)
token = box.encrypt(msg)  # nonce || ct, PyNaCl wire format
open(f"{D}/token_py", "wb").write(token)
subprocess.run([BIN, "decrypt", f"{D}/dev_sec", f"{D}/daemon_pub",
                f"{D}/token_py", f"{D}/plain_c"], check=True)
got = open(f"{D}/plain_c", "rb").read()
assert got == msg, f"C decrypt mismatch: {got!r}"
print("PASS: C decrypts PyNaCl ciphertext")

msg2 = b'{"type":"hello","clientType":"cli","protocolVersion":1}'
open(f"{D}/plain2", "wb").write(msg2)
subprocess.run([BIN, "encrypt", f"{D}/dev_sec", f"{D}/daemon_pub",
                f"{D}/plain2", f"{D}/token_c"], check=True)
token2 = open(f"{D}/token_c", "rb").read()
out = Box(dev, daemon.public_key).decrypt(token2)
assert out == msg2, f"PyNaCl decrypt mismatch: {out!r}"
out2 = Box(daemon, dev.public_key).decrypt(token2)
assert out2 == msg2
print("PASS: PyNaCl decrypts C ciphertext (both directions)")
print("ALL CRYPTO CROSS-CHECKS PASSED")
