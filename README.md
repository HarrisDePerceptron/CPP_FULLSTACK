# App Template

A C++26 web application starter built with Drogon and Xmake. The demo includes a server-rendered page, cookie-based sign-in, SQLite user storage, file uploads and downloads, and an upstream JSON endpoint.

## Getting started

Install Xmake for your platform.

**macOS** with Homebrew

```sh
brew install xmake
```

**Linux** with the official installer

```sh
curl -fsSL https://xmake.io/shget.text | bash
```

Open a new shell after installation and run `xmake --version`. For package-manager alternatives, see the [Xmake installation guide](https://xmake.io/guide/quick-start.html#installation).

Use a C++26 compiler and standard library with `std::println` support. The first build needs network access or a populated Xmake package cache. From the project root, run:

```sh
xmake f -m debug
xmake
xmake run app_server
```

Open <http://127.0.0.1:8080> and sign in with the local demo credentials `test_user` / `test_password`. Run the test suite from the project root:

```sh
xmake test
```

## Project layout

| Path | Contents |
| --- | --- |
| `src/server/` | Drogon startup, feature routes, authentication middleware, and JSON responses |
| `src/services/`, `src/db/` | Account logic, upload storage, upstream HTTP calls, and SQLite persistence |
| `src/utils/` | Configuration, logging, random IDs, and worker queue |
| `views/` | Drogon CSP templates compiled by Xmake |
| `public/` | Browser JavaScript, Tailwind source and generated CSS, and runtime uploads |
| `tests/` | Catch2 tests |

## Configuration

Edit [config.json](config.json) to change the listener or demo credentials. The shipped credentials are for local testing. [`.env.example`](.env.example) lists the optional `APP_*` settings, including `APP_JWT_SECRET` to keep existing sessions valid across server restarts. For listener settings, process environment variables take precedence over `.env` and JSON. The default C++ namespace is `App`. Set a different one with `xmake f --app_namespace=MyApp`.

## Main dependencies

- **Drogon** for HTTP routing and server-rendered views
- **SQLite, libsodium, and jwt-cpp** for account storage, password hashing, and signed sessions
- **CPR, simdjson, and Glaze** for outbound HTTP and JSON handling
- **dotenv-cpp** for loading `.env`
- **Tailwind CSS** for styling and **Catch2** for tests

## Development rules

- Keep `.hpp` files beside their `.cpp` implementations. Use four spaces and put opening braces on the declaration line.
- Keep route handlers small. Delegate persistence and outbound calls, use `co_await` for asynchronous work, `jsonResponse` for typed JSON replies, and `std::println` for console output.
- After editing a view or Tailwind source, run `npm ci` and `npm run build:css` with Node.js installed. Include the regenerated `public/demo.css` in the change.
- Keep `.env`, local databases, and runtime uploads out of commits. See [AGENTS.md](AGENTS.md) for the full repository guidelines.
