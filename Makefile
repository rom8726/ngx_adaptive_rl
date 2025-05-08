format:
	find . -type f \( -name "*.h" -o -name "*.c" \) -exec clang-format -i {} +
