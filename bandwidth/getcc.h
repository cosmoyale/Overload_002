#pragma once

inline uint64_t getcc_ns ( void )
{
	unsigned cycles_low, cycles_high;

	asm volatile (	
					"RDTSC\n\t"
					"mov %%edx, %0\n\t"
					"mov %%eax, %1\n\t" : "=r" (cycles_high), "=r" (cycles_low)::
					"%rax", "%rbx", "%rcx", "%rdx");

	return ((uint64_t)cycles_high << 32 | cycles_low);
}

inline uint64_t getcc_b ( void )
{
	unsigned cycles_low, cycles_high;

	asm volatile (	
					"CPUID\n\t"
					"RDTSCP\n\t"
					"mov %%edx, %0\n\t"
					"mov %%eax, %1\n\t" : "=r" (cycles_high), "=r" (cycles_low)::
					"%rax", "%rbx", "%rcx", "%rdx");

	return ((uint64_t)cycles_high << 32 | cycles_low);
}

inline uint64_t getcc_e ( void )
{
	unsigned cycles_low, cycles_high;

	asm volatile (
					"RDTSCP\n\t"
					"mov %%edx, %0\n\t"
					"mov %%eax, %1\n\t CPUID\n\t" : "=r" (cycles_high), "=r" (cycles_low)::
					"%rax", "%rbx", "%rcx", "%rdx");

	return ((uint64_t)cycles_high << 32 | cycles_low);
}
