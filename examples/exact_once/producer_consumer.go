// Real goroutine/channel protocol; no GC, preemption or runtime flags changed.
package main

import (
	"fmt"
	"os"
)

func main() {
	variant := "healthy"
	if len(os.Args) > 1 {
		variant = os.Args[1]
	}
	if variant != "healthy" && variant != "omit" && variant != "duplicate" {
		panic("unknown variant")
	}
	jobs := make(chan int)
	joined := make(chan []int)
	go func() {
		consumed := []int{}
		for task := range jobs {
			consumed = append(consumed, task)
		}
		joined <- consumed
	}()
	for task := 0; task < 2; task++ {
		if variant == "omit" && task == 1 {
			continue
		}
		jobs <- task
		if variant == "duplicate" && task == 1 {
			jobs <- task
		}
	}
	close(jobs)
	consumed := <-joined
	line := "EXACT_ONCE"
	for _, task := range consumed {
		line += fmt.Sprintf(" %d", task)
	}
	fmt.Println(line)
}
