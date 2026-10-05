import argparse
import base64
import hashlib
import json
from pathlib import Path

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec


def main() -> None:
    parser = argparse.ArgumentParser(description="Sign an AquaNotes OTA artifact")
    parser.add_argument("--key", required=True, type=Path)
    parser.add_argument("--artifact", required=True, type=Path)
    parser.add_argument("--signature", required=True, type=Path)
    parser.add_argument("--metadata", required=True, type=Path)
    args = parser.parse_args()

    private_key = serialization.load_pem_private_key(args.key.read_bytes(), password=None)
    if not isinstance(private_key, ec.EllipticCurvePrivateKey) or not isinstance(
        private_key.curve, ec.SECP256R1
    ):
        raise SystemExit("Signing key must be an unencrypted ECDSA P-256 private key")

    artifact = args.artifact.read_bytes()
    signature = private_key.sign(artifact, ec.ECDSA(hashes.SHA256()))
    args.signature.write_bytes(signature)
    public_der = private_key.public_key().public_bytes(
        serialization.Encoding.DER,
        serialization.PublicFormat.SubjectPublicKeyInfo,
    )
    args.metadata.write_text(
        json.dumps(
            {
                "algorithm": "ECDSA-P256-SHA256",
                "signing_key_id": hashlib.sha256(public_der).hexdigest()[:16],
                "signature_base64": base64.b64encode(signature).decode("ascii"),
            },
            indent=2,
        ),
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
