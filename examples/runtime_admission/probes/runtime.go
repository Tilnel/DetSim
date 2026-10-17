package main
import "fmt"
func main() { values := []int{1,2,3}; if values[2] != 3 { panic("bad result") }; fmt.Println("RUNTIME_OK") }
