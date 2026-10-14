class ForwardClass
{
  long value;
  int count;
};

// The parser sees the pointer without eagerly parsing the pointed-to class.
ForwardClass *class_pointer;
int main() { return 0; }
