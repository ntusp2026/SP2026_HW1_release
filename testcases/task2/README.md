# Task 2 — Same-server locking

This public task uses two clients connected to the same server.

1. Client A executes `begin 902001`.
2. Client B executes `read 902001` and must receive `>>> Locked.`.
3. Client B executes `read 902002` successfully.
4. Client A executes `abort`.
5. Client B can then read `902001` successfully.

Run it with:

```bash
python3 checker.py --task 2
```
