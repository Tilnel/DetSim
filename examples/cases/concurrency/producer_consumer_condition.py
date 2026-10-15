"""CPython threading.Condition/list protocol; default interpreter/thread settings."""
import sys
import threading

variant = sys.argv[1] if len(sys.argv) > 1 else "healthy"
assert variant in ("healthy", "omit", "duplicate")
condition = threading.Condition()
tasks = []
consumed = []
production_finished = False


def consume():
    while True:
        with condition:
            while not tasks and not production_finished:
                condition.wait()
            if not tasks:
                return
            task = tasks.pop(0)
        consumed.append(task)


worker = threading.Thread(target=consume)
worker.start()
with condition:
    for item in range(2):
        if variant == "omit" and item == 1:
            continue
        tasks.append(item)
        if variant == "duplicate" and item == 1:
            tasks.append(item)
    production_finished = True
    condition.notify()
worker.join()
print("EXACT_ONCE", *sorted(consumed), flush=True)
