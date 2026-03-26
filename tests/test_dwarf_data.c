/*
 * Test binary source - compile with: gcc -gdwarf-4 -g -o test_dwarf_data
 * test_dwarf_data.c
 *
 * This file defines various struct types to test DWARF parsing capabilities.
 */

#include <stddef.h>
#include <stdint.h>

/* Simple struct with primitive types */
struct simple_struct
{
  int a;
  long b;
  char c;
  void *ptr;
};

/* Nested struct */
struct inner_struct
{
  int x;
  int y;
};

struct outer_struct
{
  struct inner_struct inner;
  int z;
};

/* Struct with arrays */
struct array_struct
{
  int arr[10];
  char str[256];
  double matrix[4][4];
};

/* Struct with pointers */
struct ptr_struct
{
  int *int_ptr;
  struct simple_struct *struct_ptr;
  void *void_ptr;
  char **str_ptr;
};

/* Anonymous struct within struct */
struct with_anon
{
  int before;
  struct
  {
    int a;
    int b;
  } anon;
  int after;
};

/* Union type */
union my_union
{
  int i;
  float f;
  char c[4];
};

/* Typedef'd struct */
typedef struct
{
  int id;
  char name[32];
} typedef_struct;

/* Struct similar to raft_node */
struct raft_node
{
  int id;
  int state;
  void *user_data;
  struct raft_node *next;
  uint64_t match_index;
  uint64_t next_index;
  int is_voting;
};

/* Global instances for testing */
struct simple_struct g_simple = {42, 100, 'X', NULL};
struct outer_struct g_outer = {{1, 2}, 3};
struct array_struct g_array;
struct ptr_struct g_ptr;
struct with_anon g_with_anon;
union my_union g_union;
typedef_struct g_typedef;
struct raft_node g_raft_node;

int main()
{
  /* Prevent optimization */
  g_simple.a = 1;
  g_outer.z = 2;
  return 0;
}
