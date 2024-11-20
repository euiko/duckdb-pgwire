# DuckPG - DuckDB extension with postgresql wire protocol enabled

This extension is used for experimentation of adding PostgreSQL wire protocol to the DuckDB ecosystem through an extension named `duckdb_pgwire`.

## Background
DuckDB has a very unique value that bring the power of feature rich analytical database to the local environment without any external dependency. It is originally designed to be used as an embedded in-process database just like SQLite, but faster thanks to its **columnar-vectorized query execution engine** with complex query capabilities. Read more on the [DuckDB's site](https://duckdb.org/why_duckdb).

Being embedded in-process database has its own drawbacks like SQlite that once embedded it is difficult to know what happen inside the database, especially when you are start persisting data. So, in order to overcome this `DuckPG` is created. It is used to adds a PostgreSQL compatible server capabilities to the DuckDB using an extension, so you can connect to the DuckDB's database from outside the host process using PostgreSQL compatible client such as `psql`.

There are some potential use case that you can try with this extension :
- Managing in-process DuckDB from outside the process.
- Use DuckDB from the environment that are currently not supported by running on another supported host and connect via PostgreSQL protocol.

**Please note that this project is very experimental and by adding PostgreSQL compatible server means that it may contradict with the original design, try with your own risk**

The PostgreSQL wire protocol is implemented in standalone component/library named `pgwire` that heavily inspired by https://github.com/returnString/convergence and https://github.com/jeroenrinzema/psql-wire.

## Milestone

- [x] Simple query support
- [x] DuckDB extension
- [x] Simple golang client
- [ ] PGWire unit tests
- [ ] Support more data type
- [ ] Logging
- [ ] Configuration
- [ ] Session Manager
- [ ] Extended Query
- [ ] So on...

## Building and Running

Clone the repository along with the submodule with :
```bash
git clone --recurse-submodules https://github.com/euiko/duckpg.git
```

Build the extension
```bash
make -j$(nproc)
```

Open the duckdb shell or through any duckdb embedded client and load the extension
```bash
# example usage with cli
cd build/release
# somehow still require manual loading of the extension even already built onto the duckdb shell/cli
./duckdb -cmd 'load duckdb_pgwire'

```

And on the other terminal you can use psql and connect to port 15432 with ssl disabled
```bash
psql 'postgresql://localhost:15432/main' -c 'select * from generate_series(0, 100)'
```

Or you can use the postgresql driver in your language choice.
You can also run sample client in golang provided in this repo
```bash
# from the root directory
cd client/go/cmd/simple
go build && ./simple
```
