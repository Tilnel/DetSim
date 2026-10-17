// Real Rust std channel + thread lifecycle; no runtime settings are changed.
use std::{env, sync::mpsc, thread};

fn main() {
    let variant = env::args().nth(1).unwrap_or_else(|| "healthy".into());
    assert!(matches!(variant.as_str(), "healthy" | "omit" | "duplicate"));
    let (producer, consumer) = mpsc::channel();
    let worker = thread::spawn(move || consumer.into_iter().collect::<Vec<u32>>());
    for task in 0..2 {
        if variant == "omit" && task == 1 {
            continue;
        }
        producer.send(task).unwrap();
        if variant == "duplicate" && task == 1 {
            producer.send(task).unwrap();
        }
    }
    drop(producer);
    let mut consumed = worker.join().unwrap();
    consumed.sort_unstable();
    let result = consumed.iter().map(u32::to_string).collect::<Vec<_>>().join(" ");
    println!("EXACT_ONCE {}", result);
}
