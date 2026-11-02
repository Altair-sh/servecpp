# servecpp

Small HTTP/1.1 file server: download, upload, per-directory passwords.
Built on Boost.Asio coroutines and Boost.Beast. Meant to run behind a reverse proxy that handles TLS. It has built-in webpage with directory listings and upload dialog.

## Build

```sh
cmake --workflow --preset release
```

The web pages are HTML templates in [templates](templates), transpiled to C++ headers during the build by [hypertextcpp](https://github.com/kamchatka-volcano/hypertextcpp).
It has no port in the official vcpkg registry, so vcpkg builds it from the overlay port in [ports/hypertextcpp](ports/hypertextcpp), pinned to v2.0.0.
Its build downloads its dependencies with git, so the first build needs git and network access.

## Run

```sh
cp servecpp.example.toml servecpp.toml   # edit shares and passwords
build/release/servecpp                   # reads ./servecpp.toml
# or
build/release/servecpp -c /etc/servecpp/servecpp.toml
```

See [servecpp.example.toml](servecpp.example.toml) for all options.

## HTTP API

| Request | Result |
|---|---|
| `GET /` | list of shares |
| `GET /share/dir/` | directory listing with an upload form if the share has `write_password` |
| `GET /share/file` | the file, supports `Range` and `If-Modified-Since` |
| `PUT /share/file` | upload: 201 created, 204 replaced, 413 larger than `max_upload` |
| `OPTIONS /share/file` with `X-Upload-Size: <bytes>` | upload check, see below |

Passwords use HTTP Basic auth, the user name is ignored:

```sh
curl -O http://localhost:8080/public/file.zip
curl -u :password -T file.zip http://localhost:8080/public/file.zip
```

Error responses explain the reason in a plain text body, for example `wrong password` or `the file is larger than the limit of 4.0 GiB`.

### Upload check

Before uploading a file, the upload form on the listing page asks whether the upload would be accepted:

```
OPTIONS /public/file.zip
Authorization: Basic <base64 of ":password">
X-Upload-Size: 1048576
```

The server runs the same checks as for `PUT`, but receives no file and changes nothing. The body is a message the form shows to the user:

| Status | Body | Meaning |
|---|---|---|
| 200 | `file exists` | uploading replaces it, the form asks "Replace it?" |
| 404 | `file doesn't exist` | uploading creates it |
| 401 | `wrong password` | sent without `WWW-Authenticate`, so the browser doesn't show its login dialog |
| 405 | `uploads are disabled for this share` | the share has no `write_password` |
| 409 | `a directory with this name exists` | or `the parent directory doesn't exist`, `the file is being uploaded by another client`, `names ending with .uploading are reserved for uploads in progress` |
| 413 | `the file is larger than the limit of 4.0 GiB` | `X-Upload-Size` is larger than `max_upload` |
| 400 | `invalid path: ...` | the file name has characters that aren't allowed, or `X-Upload-Size` is missing or invalid |

The browser needs this because it starts sending the file with PUT request and doesn't care if server not accepts it. If server closes the connection, the request in browser crashes with no meaningful error message.

## Caddy

```
files.example.com {
    request_body {
        max_size 4GB
    }
    reverse_proxy 127.0.0.1:8080 {
        transport http {
            versions 1.1
        }
    }
}
```

Caddy streams request bodies to servecpp, so large uploads aren't buffered in memory.

## Tests

```sh
./test.sh                                    # tests build/debug/servecpp
SERVECPP=build/release/servecpp ./test.sh
```

Requires bash and curl.
