#include "defs.h"
#include "data.h"
#include "decl.h"

// Code generator for ARMv6 on Raspberry Pi
// Copyright (c) 2019 Warren Toomey, GPL3


// List of available registers and their names.
static int freereg[4];
static char *reglist[4] = { "r4", "r5", "r6", "r7" };

// Set all registers as available
void freeall_registers(void) {
  freereg[0] = freereg[1] = freereg[2] = freereg[3] = 1;
}

// Allocate a free register. Return the number of
// the register. Die if no available registers.
static int alloc_register(void) {
  for (int i = 0; i < 4; i++) {
    if (freereg[i]) {
      freereg[i] = 0;
      return (i);
    }
  }
  fatal("Out of registers");
  return (NOREG);		// Keep -Wall happy
}

// Return a register to the list of available registers.
// Check to see if it's not already there.
static void free_register(int reg) {
  if (freereg[reg] != 0)
    fatald("Error trying to free register", reg);
  freereg[reg] = 1;
}

// We have to store large integer literal values in memory.
// Keep a list of them which will be output in the postamble
#define MAXINTS 1024
int Intlist[MAXINTS];
static int Intslot = 0;

// Determine the offset of a large integer
// literal from the .L3 label. If the integer
// isn't in the list, add it.
static void set_int_offset(int val) {
  int offset = -1;

  // See if it is already there
  for (int i = 0; i < Intslot; i++) {
    if (Intlist[i] == val) {
      offset = 4 * i;
      break;
    }
  }

  // Not in the list, so add it
  if (offset == -1) {
    offset = 4 * Intslot;
    if (Intslot == MAXINTS)
      fatal("Out of int slots in set_int_offset()");
    Intlist[Intslot++] = val;
  }
  // Load r3 with this offset
  fprintf(Outfile, "\tldr\tr3, .L3+%d\n", offset);
}

// Print out the assembly preamble
void cgpreamble() {
  freeall_registers();
  fputs("\t.text\n", Outfile);
}

// Print out the assembly postamble
void cgpostamble() {

  // Print out the global variables
  fprintf(Outfile, ".L2:\n");
  for (int i = 0; i < Globs; i++) {
    if (Symtable[i].stype == S_VARIABLE)
      fprintf(Outfile, "\t.word %s\n", Symtable[i].name);
  }

  // Print out the integer literals
  fprintf(Outfile, ".L3:\n");
  for (int i = 0; i < Intslot; i++) {
    fprintf(Outfile, "\t.word %d\n", Intlist[i]);
  }
}

// Print out a function preamble
void cgfuncpreamble(int id) {
  char *name = Symtable[id].name;
  fprintf(Outfile,
	  "\t.text\n"
	  "\t.globl\t%s\n"
	  "\t.type\t%s, \%%function\n"
	  "%s:\n" "\tpush\t{fp, lr}\n"
	  "\tadd\tfp, sp, #4\n"
	  "\tsub\tsp, sp, #8\n" "\tstr\tr0, [fp, #-8]\n", name, name, name);
}

// Print out a function postamble
void cgfuncpostamble(int id) {
  cglabel(Symtable[id].endlabel);
  fputs("\tsub\tsp, fp, #4\n" "\tpop\t{fp, pc}\n" "\t.align\t2\n", Outfile);
}

// Load an integer literal value into a register.
// Return the number of the register.
int cgloadint(int value, int type) {
  // Get a new register
  int r = alloc_register();

  // If the literal value is small, do it with one instruction
  if (value <= 1000)
    fprintf(Outfile, "\tmov\t%s, #%d\n", reglist[r], value);
  else {
    set_int_offset(value);
    fprintf(Outfile, "\tldr\t%s, [r3]\n", reglist[r]);
  }
  return (r);
}

// Determine the offset of a variable from the .L2
// label. Yes, this is inefficient code.
static void set_var_offset(int id) {
  int offset = 0;
  // Walk the symbol table up to id.
  // Find S_VARIABLEs and add on 4 until
  // we get to our variable

  for (int i = 0; i < id; i++) {
    if (Symtable[i].stype == S_VARIABLE)
      offset += 4;
  }
  // Load r3 with this offset
  fprintf(Outfile, "\tldr\tr3, .L2+%d\n", offset);
}


// Load a value from a variable into a register.
// Return the number of the register
int cgloadglob(int id) {
  // Get a new register
  int r = alloc_register();

  // Get the offset to the variable
  set_var_offset(id);

  switch (Symtable[id].type) {
    case P_CHAR:
      fprintf(Outfile, "\tldrb\t%s, [r3]\n", reglist[r]);
      break;
    case P_INT:
    case P_LONG:
    case P_CHARPTR:
    case P_INTPTR:
    case P_LONGPTR:
      fprintf(Outfile, "\tldr\t%s, [r3]\n", reglist[r]);
      break;
    default:
      fatald("Bad type in cgloadglob:", Symtable[id].type);
  }
  return (r);
}

// Add two registers together and return
// the number of the register with the result
int cgadd(int r1, int r2) {
  fprintf(Outfile, "\tadd\t%s, %s, %s\n", reglist[r2], reglist[r1],
	  reglist[r2]);
  free_register(r1);
  return (r2);
}

// Subtract the second register from the first and
// return the number of the register with the result
int cgsub(int r1, int r2) {
  fprintf(Outfile, "\tsub\t%s, %s, %s\n", reglist[r1], reglist[r1],
	  reglist[r2]);
  free_register(r2);
  return (r1);
}

// Multiply two registers together and return
// the number of the register with the result
int cgmul(int r1, int r2) {
  fprintf(Outfile, "\tmul\t%s, %s, %s\n", reglist[r2], reglist[r1],
	  reglist[r2]);
  free_register(r1);
  return (r2);
}

// Divide the first register by the second and
// return the number of the register with the result
int cgdiv(int r1, int r2) {

  // To do a divide: r0 holds the dividend, r1 holds the divisor.
  // The quotient is in r0.
  fprintf(Outfile, "\tmov\tr0, %s\n", reglist[r1]);
  fprintf(Outfile, "\tmov\tr1, %s\n", reglist[r2]);
  fprintf(Outfile, "\tbl\t__aeabi_idiv\n");
  fprintf(Outfile, "\tmov\t%s, r0\n", reglist[r1]);
  free_register(r2);
  return (r1);
}

// Call a function with one argument from the given register
// Return the register with the result
int cgcall(int r, int id) {
  fprintf(Outfile, "\tmov\tr0, %s\n", reglist[r]);
  fprintf(Outfile, "\tbl\t%s\n", Symtable[id].name);
  fprintf(Outfile, "\tmov\t%s, r0\n", reglist[r]);
  return (r);
}

// Shift a register left by a constant
int cgshlconst(int r, int val) {
  fprintf(Outfile, "\tlsl\t%s, %s, #%d\n", reglist[r], reglist[r], val);
  return (r);
}

// Store a register's value into a variable
int cgstorglob(int r, int id) {

  // Get the offset to the variable
  set_var_offset(id);

  switch (Symtable[id].type) {
    case P_CHAR:
      fprintf(Outfile, "\tstrb\t%s, [r3]\n", reglist[r]);
      break;
    case P_INT:
    case P_LONG:
    case P_CHARPTR:
    case P_INTPTR:
    case P_LONGPTR:
      fprintf(Outfile, "\tstr\t%s, [r3]\n", reglist[r]);
      break;
    default:
      fatald("Bad type in cgstorglob:", Symtable[id].type);
  }
  return (r);
}

// Given a P_XXX type value, return the
// size of a primitive type in bytes.
int cgprimsize(int type) {
  if (ptrtype(type))
    return (4);
  switch (type) {
    case P_CHAR:
      return (1);
    case P_INT:
    case P_LONG:
      return (4);
    default:
      fatald("Bad type in cgprimsize:", type);
  }
  return (0);			// Keep -Wall happy
}

// Generate a global symbol
void cgglobsym(int id) {
  int typesize;
  // Get the size of the type
  typesize = cgprimsize(Symtable[id].type);

  fprintf(Outfile, "\t.data\n" "\t.globl\t%s\n", Symtable[id].name);
  switch (typesize) {
    case 1:
      fprintf(Outfile, "%s:\t.byte\t0\n", Symtable[id].name);
      break;
    case 4:
      fprintf(Outfile, "%s:\t.long\t0\n", Symtable[id].name);
      break;
    default:
      fatald("Unknown typesize in cgglobsym: ", typesize);
  }
}

// List of comparison instructions,
// in AST order: A_EQ, A_NE, A_LT, A_GT, A_LE, A_GE
static char *cmplist[] =
  { "moveq", "movne", "movlt", "movgt", "movle", "movge" };

// List of inverted jump instructions,
// in AST order: A_EQ, A_NE, A_LT, A_GT, A_LE, A_GE
static char *invcmplist[] =
  { "movne", "moveq", "movge", "movle", "movgt", "movlt" };

// Compare two registers and set if true.
int cgcompare_and_set(int ASTop, int r1, int r2) {

  // Check the range of the AST operation
  if (ASTop < A_EQ || ASTop > A_GE)
    fatal("Bad ASTop in cgcompare_and_set()");

  fprintf(Outfile, "\tcmp\t%s, %s\n", reglist[r1], reglist[r2]);
  fprintf(Outfile, "\t%s\t%s, #1\n", cmplist[ASTop - A_EQ], reglist[r2]);
  fprintf(Outfile, "\t%s\t%s, #0\n", invcmplist[ASTop - A_EQ], reglist[r2]);
  fprintf(Outfile, "\tuxtb\t%s, %s\n", reglist[r2], reglist[r2]);
  free_register(r1);
  return (r2);
}

// Generate a label
void cglabel(int l) {
  fprintf(Outfile, "L%d:\n", l);
}

// Generate a jump to a label
void cgjump(int l) {
  fprintf(Outfile, "\tb\tL%d\n", l);
}

// List of inverted branch instructions,
// in AST order: A_EQ, A_NE, A_LT, A_GT, A_LE, A_GE
static char *brlist[] = { "bne", "beq", "bge", "ble", "bgt", "blt" };

// Compare two registers and jump if false.
int cgcompare_and_jump(int ASTop, int r1, int r2, int label) {

  // Check the range of the AST operation
  if (ASTop < A_EQ || ASTop > A_GE)
    fatal("Bad ASTop in cgcompare_and_set()");

  fprintf(Outfile, "\tcmp\t%s, %s\n", reglist[r1], reglist[r2]);
  fprintf(Outfile, "\t%s\tL%d\n", brlist[ASTop - A_EQ], label);
  freeall_registers();
  return (NOREG);
}

// Widen the value in the register from the old
// to the new type, and return a register with
// this new value
int cgwiden(int r, int oldtype, int newtype) {
  // Nothing to do
  return (r);
}

// Generate code to return a value from a function
void cgreturn(int reg, int id) {
  fprintf(Outfile, "\tmov\tr0, %s\n", reglist[reg]);
  cgjump(Symtable[id].endlabel);
}

// Generate code to load the address of a global
// identifier into a variable. Return a new register
int cgaddress(int id) {
  // Get a new register
  int r = alloc_register();

  // Get the offset to the variable
  set_var_offset(id);
  fprintf(Outfile, "\tmov\t%s, r3\n", reglist[r]);
  return (r);
}

// Dereference a pointer to get the value it
// pointing at into the same register
int cgderef(int r, int type) {
  switch (type) {
    case P_CHARPTR:
      fprintf(Outfile, "\tldrb\t%s, [%s]\n", reglist[r], reglist[r]);
      break;
    case P_INTPTR:
    case P_LONGPTR:
      fprintf(Outfile, "\tldr\t%s, [%s]\n", reglist[r], reglist[r]);
      break;
  }
  return (r);
}

// Store through a dereferenced pointer
int cgstorderef(int r1, int r2, int type) {
  switch (type) {
    case P_CHAR:
      fprintf(Outfile, "\tstrb\t%s, [%s]\n", reglist[r1], reglist[r2]);
      break;
    case P_INT:
    case P_LONG:
      fprintf(Outfile, "\tstr\t%s, [%s]\n", reglist[r1], reglist[r2]);
      break;
    default:
      fatald("Can't cgstoderef on type:", type);
  }
  return (r1);
}
