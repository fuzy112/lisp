#!/usr/bin/python3

def fib(n):
	if n < 2:
		return n
	return fib(n - 1) + fib(n - 2)

print(fib(26))
