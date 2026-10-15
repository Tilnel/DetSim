"""Real CPython threading/queue lifecycle with the default interpreter settings."""
import queue
import sys
import threading

variant = sys.argv[1] if len(sys.argv) > 1 else "healthy"
assert variant in ("healthy", "omit", "duplicate")
tasks = queue.Queue()
consumed = []


def consume():
    while True:
        task = tasks.get()
        if task is None:
            return
        consumed.append(task)


worker = threading.Thread(target=consume)
worker.start()
for item in range(2):
    if variant == "omit" and item == 1:
        continue
    tasks.put(item)
    if variant == "duplicate" and item == 1:
        tasks.put(item)
tasks.put(None)
worker.join()
print("EXACT_ONCE", *sorted(consumed), flush=True)
