# WEBFIX1 — Web responsiveness correction

This build addresses the multi-second input stalls identified by DIAGNOSTICS1.

## Root cause

The browser refreshed the SD-hosted background preview every three seconds as part of `/status` processing. Each refresh streamed the JPEG synchronously through `WebServer::streamFile()`. A slow or interrupted client could therefore keep `server.handleClient()` occupied for one to three seconds, during which GPIO0 and FT6336 touch polling stopped.

The dynamic `/backgrounds/<filename>` request also passed through `onNotFound()`, producing repeated WebServer "request handler not found" messages even when the fallback handler served the file.

## Changes

- Background preview now loads only when the selected filename changes.
- Background images use the registered `/background?file=<name>` endpoint.
- Missing routes now log method, URI, and arguments as `[web-404]`.
- Responses explicitly send `Connection: close` to avoid idle keep-alive stalls.
- `/favicon.ico` has an explicit handler.
- Existing latency diagnostics remain enabled in the debug environment.

## Validation

With the webpage open, the expected ten-second diagnostic report should remain near the no-browser baseline:

- `web` maximum normally below tens of milliseconds
- button and touch poll gaps normally around 20–30 ms
- no repeating `_handleRequest(): request handler not found` entries for the background preview

A single longer web interval is acceptable while the background preview is initially loaded, but it must not repeat every three seconds.
