import base64
import json
import os
import sys


def validate_and_normalize_ca(ca_raw):
    """Turn a CUSTOM_CA value from secrets.json into a valid PEM string.

    Accepts a PEM string, a list of lines, or a base64-encoded PEM blob.
    Exits non-zero on anything that would produce an unparseable certificate,
    so the build fails loudly instead of silently shipping a broken bundle.
    """
    if not ca_raw:
        return ""

    if isinstance(ca_raw, list):
        ca_str = "\n".join(ca_raw)
    else:
        ca_str = str(ca_raw).strip()

    # Accept a base64-encoded PEM blob (some deployment tooling wraps it that
    # way to survive transport through environment variables).
    if "BEGIN CERTIFICATE" not in ca_str:
        try:
            decoded = base64.b64decode(ca_str).decode("utf-8", errors="ignore")
            if "BEGIN CERTIFICATE" in decoded:
                ca_str = decoded
        except Exception:
            pass

    # A doubled backslash in secrets.json (e.g. "\\n") decodes to a literal
    # two-character "\n" rather than a real newline. If there's no real line
    # break yet but this literal escape is present, treat it as the intended
    # line break instead of silently emitting a single-line, invalid PEM.
    if "\n" not in ca_str and "\\n" in ca_str:
        ca_str = ca_str.replace("\\r\\n", "\n").replace("\\n", "\n")

    ca_str = ca_str.replace("\r\n", "\n").strip()

    if "BEGIN CERTIFICATE" not in ca_str or "END CERTIFICATE" not in ca_str:
        print(
            "ERROR: CUSTOM_CA is not a valid PEM certificate "
            "(missing BEGIN/END CERTIFICATE header)."
        )
        sys.exit(1)

    # A real PEM certificate has the header, base64 body, and footer on
    # separate lines. Reject anything that still looks like a single-line blob
    # (stray backslashes, too few lines) rather than writing invalid PEM out to
    # custom_ca.pem.
    body_lines = [line for line in ca_str.split("\n") if line]
    if len(body_lines) < 3 or "\\" in ca_str:
        print(
            "ERROR: CUSTOM_CA does not look like a properly line-broken "
            "PEM certificate."
        )
        sys.exit(1)

    # mbedTLS's PEM parser wants a trailing newline.
    if not ca_str.endswith("\n"):
        ca_str += "\n"

    return ca_str


def write_custom_ca(config):
    """Materialize main/certs/custom_ca.pem for the mbedtls custom bundle.

    CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE_PATH points at main/certs, so the
    CA is merged into the compiled-in bundle at build time and a single
    esp_crt_bundle_attach trusts both it and the default public roots — no
    branching TLS setup in the HTTP client. gen_crt_bundle.py needs at least
    one .pem in that directory, so write a placeholder when no CA is set.
    """
    certs_dir = os.path.join(os.getcwd(), "main", "certs")
    os.makedirs(certs_dir, exist_ok=True)
    custom_ca_path = os.path.join(certs_dir, "custom_ca.pem")

    if config.get("CUSTOM_CA"):
        contents = validate_and_normalize_ca(config["CUSTOM_CA"])
    else:
        contents = "# No custom CA configured - using default bundle only\n"

    with open(custom_ca_path, "w") as f:
        f.write(contents)


def generate_cmake_secrets(output_path):
    secrets_file = os.path.join(os.getcwd(), "secrets.json")
    placeholders_file = os.path.join(os.getcwd(), "secrets_place.json")

    config = {}

    if os.path.exists(secrets_file):
        try:
            with open(secrets_file, "r") as f:
                config = json.load(f)
        except (json.JSONDecodeError, FileNotFoundError) as e:
            print(f"Warning: Could not load or parse secrets.json: {e}")
    elif os.path.exists(placeholders_file):
        try:
            with open(placeholders_file, "r") as f:
                config = json.load(f)
            print(f"Note: Using fallback secrets from {placeholders_file}")
        except (json.JSONDecodeError, FileNotFoundError) as e:
            print(f"Warning: Could not load or parse {placeholders_file}: {e}")

    cmake_content = "# Generated secrets overrides\n"

    def escape_cmake(value):
        return str(value).replace("\\", "\\\\").replace('"', '\\"')

    # We use the variable names expected by main/CMakeLists.txt
    if "WIFI_SSID" in config:
        cmake_content += f'set(VAL_WIFI_SSID "{escape_cmake(config["WIFI_SSID"])}")\n'
    if "WIFI_PASSWORD" in config:
        cmake_content += (
            f'set(VAL_WIFI_PASSWORD "{escape_cmake(config["WIFI_PASSWORD"])}")\n'
        )
    if "REMOTE_URL" in config:
        cmake_content += f'set(VAL_REMOTE_URL "{escape_cmake(config["REMOTE_URL"])}")\n'

    write_custom_ca(config)

    with open(output_path, "w") as f:
        f.write(cmake_content)


if __name__ == "__main__":
    output_path = "secrets.cmake"
    if len(sys.argv) > 1:
        output_path = sys.argv[1]
    generate_cmake_secrets(output_path)
