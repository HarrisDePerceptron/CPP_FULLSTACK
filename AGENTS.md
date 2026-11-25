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

Use four spaces and place opening braces on the declaration line. Use `.hpp` for C++ headers and `.cpp` for implementations. Keep route handlers small and delegate persistence and outbound calls to their modules. Always use `co_await` for asynchronous application code and `std::println` for console output, including logs. Use `apiOk` and `apiError` for typed API responses.

Prefer values for direct ownership and references for required borrows. Use `std::optional<std::reference_wrapper<T>>` for optional borrows and smart pointers for explicit heap ownership. Avoid raw pointers in application interfaces. Adapt third-party APIs that require raw pointers at the integration boundary.

## Database and Service Layers

- Define persistent fields in a snake_case aggregate with a static `table_name`. Wrap it in `DaoRecord<Fields>` and derive the entity DAO from `BaseDao<Derived, Record>`. Use field annotations for keys, constraints, indexes, defaults, and foreign references. See the [README database example](README.md#database-example).
- Open the database backend once at startup and pass it to each DAO's async `init(database)`. Initialize parent DAOs before DAOs that reference them. Use the base DAO's coroutine CRUD methods and typed filters for ordinary queries. Keep custom SQL in the DAO, and bind runtime values through `executeSql`.
- Build list queries with `dao.find()` or `dao.find(tx)` and execute them with `co_await … .collect()`. The default is 100 rows at offset 0. Set a different window with `.limit()` and `.offset()`. Use `.where<Member>(value)` for a typed filter, `.where(predicate)` for a composed predicate, and `.matching(filters)` for runtime `EqualityFilters`. Add projection, sorting, and foreign loading with `.select()`, `.selectAll()`, `.orderBy()`, and `.withForeign()`. List queries exclude soft-deleted rows unless you add `.includeDeleted()`.
- Finish ID lookups and counts with `.collect()` too. Use `findById(id)` for an optional full record and `count()` for the total across all matching active rows. Pass a transaction as the first argument when needed. Both builders support `.includeDeleted()`.
- Use `createMany(records)` and `updateMany(records)` for atomic batches. Pass the existing transaction first to scope the batch to a savepoint. Create returns keys in input order. Update returns false and rolls back the batch if any target is missing or soft-deleted.
- Give each service its initialized DAO by value and keep business logic in the service. Register services in the server's `Services` container. Route handlers should validate requests, `co_await` service methods, and build responses. Keep database queries and persistence records inside the service and DAO layers.

```cpp
auto posts = co_await postDao.find()
    .where<&SocialPost::author_id>(userId)
    .select<&SocialPost::created_at>()
    .orderBy<&SocialPost::created_at>(SortDirection::desc)
    .limit(20)
    .collect();
```

## Testing

- Run the Catch2 suite with `xmake test --output-on-failure`. Name test files `*_tests.cpp` and describe the observable behavior in each test case name.
- Give each test case one behavior or a closely related scenario. Use separate cases for successful operations, expected errors, and edge conditions. Keep related assertions together when they verify the same outcome.
- Keep cases independent of execution order. Share fixture setup where it reduces duplication, and give each case isolated mutable state, such as its own in-memory SQLite database or temporary directory.
- Use loopback fixtures for HTTP integration tests and upstream calls. Smoke-test changes to the demo page and verify returned download links when applicable.

## Commits and Pull Requests

Use a short imperative subject with a prefix such as `feat: add upload route`. Summarize the change, report verification results, and link a related issue when one exists.
