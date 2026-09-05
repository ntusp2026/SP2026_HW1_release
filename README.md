# SP2026_HW1_release

Starter files and public test cases for **System Programming 2026 Coding Assignment 1 — csieLedger**.

Read the complete assignment specification on HackMD before implementation.

Specification: `{HACKMD_SPEC_LINK}`

## Repository contents

```text
.
├── Makefile
├── README.md
├── accountRecord
├── checker.py
├── server.c
├── server.h
└── testcases/
```

`accountRecord` is a binary file containing 20 records with the following layout:

```c
typedef struct {
    int id;
    int balance;
} account_record;
```

## Public testcase judge

Compile your server first:

```bash
make
```

Run all public tasks:

```bash
python3 checker.py
```

Run selected tasks:

```bash
python3 checker.py --task 1-1 1-2
```

Valid tasks are:

```text
1-1 1-2 1-3 1-4 2
```

The checker temporarily modifies `accountRecord` and restores the original file before exiting. Do not run another server from this directory while the checker is running.

Passing the public judge does not guarantee full credit.
