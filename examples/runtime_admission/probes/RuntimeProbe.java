public class RuntimeProbe {
    public static void main(String[] args) {
        int[] values = {1, 2, 3};
        if (values[2] != 3) throw new AssertionError("bad result");
        System.out.println("RUNTIME_OK");
    }
}
