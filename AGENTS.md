# Repository Guidelines

## Project Structure

- `src/main.cpp` starts the Drogon server. `src/server/` contains startup and JSON responses, with feature routes in `src/server/routes/` and JWT handling in `src/server/auth/`.
- `src/services/` handles account logic, uploads, and upstream content. `src/db/` owns SQLite persistence. `src/utils/` contains configuration, logging, random ID generation, and the worker queue.
- Keep each C++ header beside its implementation and use paths relative to `src/` when including headers from another directory.
- `views/` holds Drogon CSP templates compiled by Xmake. `public/` holds JavaScript, Tailwind source (`tailwind.css`), generated stylesheet (`demo.css`), and runtime uploads. `tests/` contains the Catch2 suite.
- Xmake writes compiled output to `build/` and keeps local build configuration in `.xmake/`.

## Build and Run

Run commands from the project root with Xmake and a C++26 compiler and standard library that support `std::println`.

- `xmake f -m debug` or `xmake f -m release` selects the build mode.
- Set a custom C++ namespace with `xmake f --app_namespace=MyApp`. The default is `App`.
- `xmake` resolves dependencies and builds the server. First-time dependency resolution requires network access or a populated Xmake cache.
- `xmake run app_server` starts the server at `http://127.0.0.1:8080` with the default `config.json`. The shipped config sets the local demo credentials with `test_user` and `test_password`. When both are omitted, the server generates demo credentials on first launch and prints them. Startup applies configured credentials to the demo account in `.local/`. Use the shipped credentials only for local testing.
- After editing the page or Tailwind source, run `npm ci` and `npm run build:css`. Commit the regenerated `public/demo.css`.

The optional `.env` supplies `APP_JWT_SECRET`. See `.env.example` for the format. An existing environment variable takes precedence. Without a configured key, a server restart invalidates previously issued tokens. Keep `.env`, database files, and uploaded content out of commits.

Listener settings resolve from the process environment, then `.env`, then `config.json`. Use `APP_BIND_ADDRESS` for `bind_address` (defaults to `127.0.0.1`), `APP_PORT` for the required `port` field, and `APP_MAX_CONNECTIONS` for `max_connections` (defaults to `64`). Update existing deployments to use `APP_JWT_SECRET` and sign in again after upgrading.

## Coding Style

Use four spaces and place opening braces on the declaration line. Use `.hpp` for C++ headers and `.cpp` for implementations. Keep route handlers small and delegate persistence and outbound calls to their modules. Always use `co_await` for asynchronous application code and `std::println` for console output, including logs. Use `jsonResponse` for typed API responses.

## Testing

Run `xmake test` to run Catch2. Name test files `*_tests.cpp` and describe observable behavior in test case names. Use temporary state and a loopback fixture for upstream calls. Smoke-test HTTP changes through the demo page and verify returned download links.

## Commits and Pull Requests

Use a short imperative subject with a prefix such as `feat: add upload route`. Summarize the change, report verification results, and link a related issue when one exists.
