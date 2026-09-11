# Certificates

This directory holds the PEM files embedded into firmware at build time.
**Never commit `.pem` files to git.**

## Files

| File | Purpose | Embedded as |
|------|---------|-------------|
| `ca.pem` | CA certificate (verifies broker) | `_binary_ca_pem_start` |
| `cert.pem` | Device client certificate | `_binary_cert_pem_start` |
| `key.pem` | Device private key | `_binary_key_pem_start` |

## Generate dev certs (self-signed)

```bash
# CA key + cert
openssl req -x509 -newkey rsa:2048 -keyout ca.key -out ca.pem \
  -days 3650 -nodes -subj "/CN=Padena Tank Sensor CA"

# Device key + CSR + cert (signed by CA)
openssl req -newkey rsa:2048 -keyout key.pem -out device.csr \
  -nodes -subj "/CN=tank-sensor-01"

openssl x509 -req -in device.csr -CA ca.pem -CAkey ca.key \
  -CAcreateserial -out cert.pem -days 3650

rm ca.key device.csr ca.srl
```

## Production certs

Use your VPS's Mosquitto CA to sign per-device CSRs.
Set CN to match what Mosquitto's ACL maps to (e.g. `tank-sensor-01`).

## Important

- **Always use `-nodes`** (no DES) when generating keys. Encrypted private keys
  require `credentials.authentication.key_password` in the MQTT config, and
  passphrase management on headless embedded devices is painful.
- `EMBED_TXTFILES` in CMakeLists.txt null-terminates the blobs, which is what
  `esp_mqtt_client_config_t` expects for cert/key PEM strings. Do not use
  `EMBED_FILES` — it produces raw byte arrays without null termination.
