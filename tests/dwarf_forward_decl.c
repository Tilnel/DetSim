typedef struct ForwardResolved ForwardAlias;
typedef struct ForwardMissing MissingAlias;
typedef struct ForwardResolved ForwardResolved;
typedef struct ForwardMissing ForwardMissing;
ForwardAlias *forward_pointer;
MissingAlias *opaque_pointer;
ForwardResolved *same_name_pointer;
ForwardMissing *same_name_opaque_pointer;
int main(void) { return 0; }
