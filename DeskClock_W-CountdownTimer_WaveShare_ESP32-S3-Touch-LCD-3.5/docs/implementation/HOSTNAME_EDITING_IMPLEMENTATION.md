# Editable Network Hostname

The Network card now includes a hostname field and a dedicated **Save and Reboot** button.

Validation rules:

- 1 to 32 characters
- letters, numbers, and hyphens only
- no leading or trailing hyphen
- uppercase input is normalized to lowercase
- enter only the hostname; `.local` is appended for display

The hostname is stored in the `deskclock` Preferences namespace under `hostname`.
It is applied before Wi-Fi connects and is used for DHCP station naming and mDNS.
The default and invalid-value fallback is `countdown`.

Web endpoint:

- `POST /network/hostname`
- form field: `hostname`
- valid requests save and reboot
- invalid requests return HTTP 400 JSON without changing the saved hostname
