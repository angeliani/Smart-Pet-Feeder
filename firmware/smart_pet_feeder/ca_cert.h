#pragma once
// Root CA broker MQTT. Ganti isi di bawah dengan sertifikat root CA broker kalian
// (mis. dari dashboard broker/penyedia cloud). Sertifikat CA bersifat publik, aman di-commit.
static const char ROOT_CA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
ISI_DENGAN_ROOT_CA_BROKER
-----END CERTIFICATE-----
)EOF";
