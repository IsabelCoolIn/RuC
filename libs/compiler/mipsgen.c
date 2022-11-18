/*
 *	Copyright 2021 Andrey Terekhov, Ivan S. Arkhipov
 *
 *	Licensed under the Apache License, Version 2.0 (the "License");
 *	you may not use this file except in compliance with the License.
 *	You may obtain a copy of the License at
 *
 *		http://www.apache.org/licenses/LICENSE-2.0
 *
 *	Unless required by applicable law or agreed to in writing, software
 *	distributed under the License is distributed on an "AS IS" BASIS,
 *	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *	See the License for the specific language governing permissions and
 *	limitations under the License.
 */

#include "mipsgen.h"
#include "AST.h"
#include "hash.h"
#include "operations.h"
#include "tree.h"
#include "uniprinter.h"


static const size_t BUFFER_SIZE = 65536; /**< Размер буфера для тела функции */
static const size_t HASH_TABLE_SIZE = 1024; /**< Размер хеш-таблицы для смещений и регистров */
static const bool IS_ON_STACK = true; /**< Хранится ли переменная на стеке */

static const size_t WORD_LENGTH = 4;	  /**< Длина слова данных */
static const size_t HALF_WORD_LENGTH = 2; /**< Длина половины слова данных */

static const size_t LOW_DYN_BORDER = 0x10010000; /**< Нижняя граница динамической памяти */
static const size_t HEAP_DISPL = 8000; /**< Смещение кучи относительно глобальной памяти */

static const size_t SP_SIZE = 4; /**< Размер регистра $sp для его сохранения */
static const size_t RA_SIZE = 4; /**< Размер регистра $ra для его сохранения */

static const size_t TEMP_FP_REG_AMOUNT = 12; /**< Количество временных регистров
												 для чисел с плавающей точкой */
static const size_t TEMP_REG_AMOUNT = 8; /**< Количество обычных временных регистров */
static const size_t ARG_REG_AMOUNT = 4; /**< Количество регистров-аргументов для функций */

static const size_t PRESERVED_REG_AMOUNT = 8; /**< Количество сохраняемых регистров общего назначения */
static const size_t PRESERVED_FP_REG_AMOUNT = 10; /**< Количество сохраняемых регистров с плавающей точкой */

static const bool FROM_LVALUE = 1; /**< Получен ли rvalue из lvalue */

/**< Смещение в стеке для сохранения оберегаемых регистров,
	 без учёта оптимизаций */
static const size_t FUNC_DISPL_PRESEREVED = /* за $sp */ 4 + /* за $ra */ 4 +
											/* fs0-fs10 (одинарная точность): */ 5 * 4 + /* s0-s7: */ 8 * 4 +
											/* a0-a3: */ 4 * 4;

// Назначение регистров взято из документации SYSTEM V APPLICATION BINARY INTERFACE MIPS RISC Processor, 3rd Edition
typedef enum MIPS_REGISTER
{
	R_ZERO, /**< Always has the value 0 */
	R_AT,	/**< Temporary, generally used by assembler */

	R_V0,
	R_V1, /**< Used for expression evaluations and to hold the integer
			  and pointer type function return values */

	R_A0,
	R_A1,
	R_A2,
	R_A3, /**< Used for passing arguments to functions; values are not
			  preserved across function calls */

	R_T0,
	R_T1,
	R_T2,
	R_T3,
	R_T4,
	R_T5,
	R_T6,
	R_T7, /**< Temporary registers used for expression evaluation;
			  values are not preserved across function calls */

	R_S0,
	R_S1,
	R_S2,
	R_S3,
	R_S4,
	R_S5,
	R_S6,
	R_S7, /**< Saved registers; values are preserved across function calls */

	R_T8,
	R_T9, /**< Temporary registers used for expression evaluations;
			  values are not preserved across function calls.  When
			  calling position independent functions $25 (R_T9) must contain
			  the address of the called function */

	R_K0,
	R_K1, /**< Used only by the operating system */

	R_GP, /**< Global pointer and context pointer */
	R_SP, /**< Stack pointer */
	R_FP, /**< Saved register (like s0-s7) or frame pointer */
	R_RA, /**< Return address. The return address is the location to
			  which a function should return control */

	// Регистры для работы с числами с плавающей точкой
	// Для чисел с двойной точностью используется пара регистров:
	// - регистр с чётным номером содержит младшие 32 бита числа;
	// - регистр с нечётным номером содержит старшие 32 бита числа.
	R_FV0,
	R_FV1,
	R_FV2,
	R_FV3, /**< used to hold floating-point type function results;
			   single-precision uses $f0 and double-precision uses
			   the register pair $f0..$f1 */

	R_FA0,
	R_FA1,
	R_FA2,
	R_FA3, /**< Used for passing arguments to functions */

	R_FT0,
	R_FT1,
	R_FT2,
	R_FT3,
	R_FT4,
	R_FT5,
	R_FT6,
	R_FT7,
	R_FT8,
	R_FT9,
	R_FT10,
	R_FT11, /**< Temporary registers */

	R_FS0,
	R_FS1,
	R_FS2,
	R_FS3,
	R_FS4,
	R_FS5,
	R_FS6,
	R_FS7,
	R_FS8,
	R_FS9,
	R_FS10,
	R_FS11 /**< Saved registers; their values are preserved across function calls */
} mips_register_t;


// Назначение команд взято из документации MIPS® Architecture for Programmers
// Volume II-A: The MIPS32® Instruction
// Set Manual 2016
typedef enum INSTRUCTION
{
	IC_MIPS_MOVE, /**< MIPS Pseudo-Instruction. Move the contents of one register to another */
	IC_MIPS_LI,	  /**< MIPS Pseudo-Instruction. Load a constant into a register */
	IC_MIPS_NOT,  /**< MIPS Pseudo-Instruction. Flips the bits of the source register and
					  stores them in the destination register (не из вышеуказанной книги) */

	IC_MIPS_ADDI, /**< To add a constant to a 32-bit integer. If overflow occurs, then trap */
	IC_MIPS_SLL,  /**< To left-shift a word by a fixed number of bits */
	IC_MIPS_SRA,  /**< To execute an arithmetic right-shift of a word by a fixed number of bits */
	IC_MIPS_ANDI, /**< To do a bitwise logical AND with a constant */
	IC_MIPS_XORI, /**< To do a bitwise logical Exclusive OR with a constant */
	IC_MIPS_ORI,  /**< To do a bitwise logical OR with a constant */

	IC_MIPS_ADD,  /**< To add 32-bit integers. If an overflow occurs, then trap */
	IC_MIPS_SUB,  /**< To subtract 32-bit integers. If overflow occurs, then trap */
	IC_MIPS_MUL,  /**< To multiply two words and write the result to a GPR */
	IC_MIPS_DIV,  /**< DIV performs a signed 32-bit integer division, and places
					   the 32-bit quotient result in the destination register */
	IC_MIPS_MOD,  /**< MOD performs a signed 32-bit integer division, and places
					   the 32-bit remainder result in the destination register.
					   The remainder result has the same sign as the dividend */
	IC_MIPS_SLLV, /**< To left-shift a word by a variable number of bits */
	IC_MIPS_SRAV, /**< To execute an arithmetic right-shift of a word by a variable number of bits */
	IC_MIPS_AND,  /**< To do a bitwise logical AND */
	IC_MIPS_XOR,  /**< To do a bitwise logical Exclusive OR */
	IC_MIPS_OR,	  /**< To do a bitwise logical OR */

	IC_MIPS_SW, /**< To store a word to memory */
	IC_MIPS_LW, /**< To load a word from memory as a signed value */

	IC_MIPS_JR,	 /**< To execute a branch to an instruction address in a register */
	IC_MIPS_JAL, /**< To execute a procedure call within the current 256MB-aligned region */
	IC_MIPS_J,	 /**< To branch within the current 256 MB-aligned region */

	IC_MIPS_BLEZ, /**< Branch on Less Than or Equal to Zero.
					  To test a GPR then do a PC-relative conditional branch */
	IC_MIPS_BLTZ, /**< Branch on Less Than Zero.
					  To test a GPR then do a PC-relative conditional branch */
	IC_MIPS_BGEZ, /**< Branch on Greater Than or Equal to Zero.
					  To test a GPR then do a PC-relative conditional branch */
	IC_MIPS_BGTZ, /**< Branch on Greater Than Zero.
					  To test a GPR then do a PC-relative conditional branch */
	IC_MIPS_BEQ,  /**< Branch on Equal.
					  To compare GPRs then do a PC-relative conditional branch */
	IC_MIPS_BNE,  /**< Branch on Not Equal.
					  To compare GPRs then do a PC-relative conditional branch */

	IC_MIPS_LA, /**< Load the address of a named memory
					location into a register (не из вышеуказанной книги)*/

	IC_MIPS_NOP, /**<To perform no operation */

	/** Floating point operations. Single precision. */
	IC_MIPS_ADD_S, /**< To add FP values. */
	IC_MIPS_SUB_S, /**< To subtract FP values. */
	IC_MIPS_MUL_S, /**< To multiply FP values. */
	IC_MIPS_DIV_S, /**< To divide FP values. */

	IC_MIPS_S_S, /**< MIPS Pseudo instruction. To store a doubleword from an FPR to memory. */
	IC_MIPS_L_S, /**< MIPS Pseudo instruction. To load a doubleword from memory to an FPR. */

	IC_MIPS_LI_S, /**< MIPS Pseudo-Instruction. Load a FP constant into a FPR. */

	IC_MIPS_MOV_S, /**< The value in first FPR is placed into second FPR. */

	IC_MIPS_MFC_1,	/**< Move word from Floating Point.
						To copy a word from an FPU (CP1) general register to a GPR. */
	IC_MIPS_MFHC_1, /**< To copy a word from the high half of an FPU (CP1)
						general register to a GPR. */

	IC_MIPS_CVT_D_S, /**< To convert an FP value to double FP. */
	IC_MIPS_CVT_S_W, /**< To convert fixed point value to single FP. */
	IC_MIPS_CVT_W_S, /**< To convert single FP to fixed point value */
} mips_instruction_t;


typedef enum LABEL
{
	L_FUNC,		   /**< Тип метки -- вход в функцию */
	L_NEXT,		   /**< Тип метки -- следующая функция */
	L_FUNCEND,	   /**< Тип метки -- выход из функции */
	L_STRING,	   /**< Тип метки -- строка */
	L_ELSE,		   /**< Тип метки -- переход по else */
	L_END,		   /**< Тип метки -- переход в конец конструкции */
	L_BEGIN_CYCLE, /**< Тип метки -- переход в начало цикла */
} mips_label_t;

typedef struct label
{
	mips_label_t kind;
	size_t num;
} label;

typedef struct information
{
	syntax *sx; /**< Структура syntax с таблицами */

	size_t max_displ;	 /**< Максимальное смещение от $sp */
	size_t global_displ; /**< Смещение от $gp */

	hash displacements; /**< Хеш таблица с информацией о расположении идентификаторов:
							@c key		 - ссылка на таблицу идентификаторов
							@c value[0]	 - флаг, лежит ли переменная на стеке или в регистре
							@c value[1]  - смещение или номер регистра */

	mips_register_t next_register; /**< Следующий обычный регистр для выделения */
	mips_register_t next_float_register; /**< Следующий регистр с плавающей точкой для выделения */

	size_t label_num;						/**< Номер метки */
	label label_else;						/**< Метка перехода на else */
	label label_continue;					/**< Метка continue */
	label label_break;						/**< Метка break */
	size_t curr_function_ident; 			/**< Идентификатор текущей функций */

	bool registers[24]; /**< Информация о занятых регистрах */

	item_t displ; /**< Смещение */
} information;

/** Kinds of lvalue */
typedef enum LVALUE_KIND
{
	LVALUE_KIND_STACK,
	LVALUE_KIND_REGISTER,
} lvalue_kind_t;

typedef struct lvalue
{
	lvalue_kind_t kind;		  /**< Value kind */
	mips_register_t base_reg; /**< Base register */
	union					  /**< Value location */
	{
		item_t reg_num; /**< Register where the value is stored */
		item_t displ;	/**< Stack displacement where the value is stored */
	} loc;
	item_t type; /**< Value type */
} lvalue;

/** Kinds of rvalue */
typedef enum RVALUE_KIND
{
	RVALUE_KIND_CONST, // Значит, запомнили константу и потом обработали её
	RVALUE_KIND_REGISTER,
	RVALUE_KIND_VOID,
} rvalue_kind_t;

typedef struct rvalue
{
	rvalue_kind_t kind; /**< Value kind */
	item_t type;		/**< Value type */
	bool from_lvalue;	/**< Was the rvalue instance formed from lvalue */
	union
	{
		item_t reg_num;	  /**< Where the value is stored */
		item_t int_val;	  /**< Value of integer (character, boolean) literal */
		double float_val; /**< Value of floating literal */
		item_t str_index; /**< Index of pre-declared string */
		// TODO: остальные типы (включая сложные: массивы/структуры)
	} val;
} rvalue;

static const rvalue rvalue_one = { .kind = RVALUE_KIND_CONST, .type = TYPE_INTEGER, .val.int_val = 1 };
static const rvalue rvalue_negative_one = { .kind = RVALUE_KIND_CONST, .type = TYPE_INTEGER, .val.int_val = -1 };


static lvalue emit_lvalue(information *const info, const node *const nd);
static rvalue emit_expression(information *const info, const node *const nd);
static void emit_statement(information *const info, const node *const nd);
static lvalue emit_store_of_rvalue(information *const info, const rvalue rval, const lvalue lval);
static rvalue emit_load_of_lvalue(information *const info, const lvalue lval);
static void emit_store_rvalue_to_rvalue(information *const info, const rvalue destination, const rvalue source);
static rvalue emit_unary_expression_rvalue(information *const info, const node *const nd);
static void emit_label(information *const info, const label lbl);
static rvalue emit_binary_operation(information *const info, rvalue rval1, rvalue rval2, const binary_t operation);


/**
 * Takes the first free register
 *
 * @param	info				Codegen info (?)
 *
 * @return	Register
 */
static mips_register_t get_register(information *const info)
{
	// Ищем первый свободный регистр
	mips_register_t i = 0;
	for (; i < TEMP_REG_AMOUNT; i++)
	{
		if (!info->registers[i])
		{
			break;
		}
	}

	assert(i != TEMP_REG_AMOUNT);

	// Занимаем его
	info->registers[i] = true;

	return i + R_T0;
}

/**
 * Takes the first free floating point register
 *
 * @param	info				Codegen info (?)
 *
 * @return	Register
 */
static mips_register_t get_float_register(information *const info)
{
	// Ищем первый свободный регистр
	mips_register_t i = 8;
	for (; i < TEMP_FP_REG_AMOUNT + TEMP_REG_AMOUNT; i += 2 /* т.к. операции с одинарной точностью */)
	{
		if (!info->registers[i])
		{
			break;
		}
	}

	assert(i != TEMP_FP_REG_AMOUNT + TEMP_REG_AMOUNT);

	// Занимаем его
	info->registers[i] = true;

	return i + R_FT0 - /* за индекс R_FT0 в info->registers */ 10;
}

/**
 * Free register
 * 
 * 
*/
static void free_register(information *const info, const mips_register_t reg)
{
	switch (reg)
	{
		case R_T0:
		case R_T1:
		case R_T2:
		case R_T3:
		case R_T4:
		case R_T5:
		case R_T6:
		case R_T7:
			if (info->registers[reg - R_T0])
			{
				// Регистр занят => освобождаем
				info->registers[reg - R_T0] = false;
			}
			return;

		case R_T8:
		case R_T9:
			if (info->registers[reg - R_T8 + /* индекс R_T8 в info->registers */ 8])
			{
				// Регистр занят => освобождаем
				info->registers[reg - R_T8 + 8] = false;
			}
			return;

		case R_FT0:
		case R_FT1:
		case R_FT2:
		case R_FT3:
		case R_FT4:
		case R_FT5:
		case R_FT6:
		case R_FT7:
		case R_FT8:
		case R_FT9:
		case R_FT10:
		case R_FT11:
			if (info->registers[reg - R_FT0 + /* индекс R_FT0 в info->registers */ 10])
			{
				// Регистр занят => освобождаем
				info->registers[reg - R_FT0 + 10] = false;
			}
			return;

		default: // Не временный регистр => освобождать не надо
			return;
	}
}

/**
 * Free register occupied by rvalue
 *
 * @param	info				Codegen info (?)
 * @param	rval				Rvalue to be freed
 */
static void free_rvalue(information *const info, const rvalue rval)
{
	if ((rval.kind == RVALUE_KIND_REGISTER) && (!rval.from_lvalue))
	{
		free_register(info, rval.val.reg_num);
	}
}

/**
 * Reverse binary logic operation
 *
 * @param	operation			Operation to reverse
 *
 * @return	Reversed binary operation
 */
static binary_t reverse_logic_command(const binary_t operation)
{
	assert(operation >= BIN_LT);
	assert(operation <= BIN_NE);

	switch (operation)
	{
		case BIN_LT:
			return BIN_GE;
		case BIN_GT:
			return BIN_LE;
		case BIN_LE:
			return BIN_GT;
		case BIN_GE:
			return BIN_LT;
		case BIN_EQ:
			return BIN_NE;
		default: // BIN_NE
			return BIN_EQ;
	}
}

/** Get MIPS assembler binary instruction from binary_t type
 *
 * @param	operation_type		Type of operation in AST
 * @param	is_imm				@c True if the instruction is immediate, @c False otherwise
 *
 * @return	MIPS binary instruction
 */
static mips_instruction_t get_bin_instruction(const binary_t operation_type, const bool is_imm)
{
	switch (operation_type)
	{
		case BIN_ADD_ASSIGN:
		case BIN_ADD:
			return (is_imm) ? IC_MIPS_ADDI : IC_MIPS_ADD;

		case BIN_SUB_ASSIGN:
		case BIN_SUB:
			return (is_imm) ? IC_MIPS_ADDI : IC_MIPS_SUB;

		case BIN_MUL_ASSIGN:
		case BIN_MUL:
			return IC_MIPS_MUL;

		case BIN_DIV_ASSIGN:
		case BIN_DIV:
			return IC_MIPS_DIV;

		case BIN_REM_ASSIGN:
		case BIN_REM:
			return IC_MIPS_MOD;

		case BIN_SHL_ASSIGN:
		case BIN_SHL:
			return (is_imm) ? IC_MIPS_SLL : IC_MIPS_SLLV;

		case BIN_SHR_ASSIGN:
		case BIN_SHR:
			return (is_imm) ? IC_MIPS_SRA : IC_MIPS_SRAV;

		case BIN_AND_ASSIGN:
		case BIN_AND:
			return (is_imm) ? IC_MIPS_ANDI : IC_MIPS_AND;

		case BIN_XOR_ASSIGN:
		case BIN_XOR:
			return (is_imm) ? IC_MIPS_XORI : IC_MIPS_XOR;

		case BIN_OR_ASSIGN:
		case BIN_OR:
			return (is_imm) ? IC_MIPS_ORI : IC_MIPS_OR;

		case BIN_EQ:
			return IC_MIPS_BEQ;
		case BIN_NE:
			return IC_MIPS_BNE;
		case BIN_GT:
			return IC_MIPS_BGTZ;
		case BIN_LT:
			return IC_MIPS_BLTZ;
		case BIN_GE:
			return IC_MIPS_BGEZ;
		case BIN_LE:
			return IC_MIPS_BLEZ;

		default:
			return IC_MIPS_NOP;
	}
}

static void mips_register_to_io(universal_io *const io, const mips_register_t reg)
{
	switch (reg)
	{
		case R_ZERO:
			uni_printf(io, "$0");
			break;
		case R_AT:
			uni_printf(io, "$at");
			break;

		case R_V0:
			uni_printf(io, "$v0");
			break;
		case R_V1:
			uni_printf(io, "$v1");
			break;

		case R_A0:
			uni_printf(io, "$a0");
			break;
		case R_A1:
			uni_printf(io, "$a1");
			break;
		case R_A2:
			uni_printf(io, "$a2");
			break;
		case R_A3:
			uni_printf(io, "$a3");
			break;

		case R_T0:
			uni_printf(io, "$t0");
			break;
		case R_T1:
			uni_printf(io, "$t1");
			break;
		case R_T2:
			uni_printf(io, "$t2");
			break;
		case R_T3:
			uni_printf(io, "$t3");
			break;
		case R_T4:
			uni_printf(io, "$t4");
			break;
		case R_T5:
			uni_printf(io, "$t5");
			break;
		case R_T6:
			uni_printf(io, "$t6");
			break;
		case R_T7:
			uni_printf(io, "$t7");
			break;

		case R_S0:
			uni_printf(io, "$s0");
			break;
		case R_S1:
			uni_printf(io, "$s1");
			break;
		case R_S2:
			uni_printf(io, "$s2");
			break;
		case R_S3:
			uni_printf(io, "$s3");
			break;
		case R_S4:
			uni_printf(io, "$s4");
			break;
		case R_S5:
			uni_printf(io, "$s5");
			break;
		case R_S6:
			uni_printf(io, "$s6");
			break;
		case R_S7:
			uni_printf(io, "$s7");
			break;

		case R_T8:
			uni_printf(io, "$t8");
			break;
		case R_T9:
			uni_printf(io, "$t9");
			break;

		case R_K0:
			uni_printf(io, "$k0");
			break;
		case R_K1:
			uni_printf(io, "$k1");
			break;

		case R_GP:
			uni_printf(io, "$gp");
			break;
		case R_SP:
			uni_printf(io, "$sp");
			break;
		case R_FP:
			uni_printf(io, "$fp");
			break;
		case R_RA:
			uni_printf(io, "$ra");
			break;

		case R_FV0:
			uni_printf(io, "$f0");
			break;
		case R_FV1:
			uni_printf(io, "$f1");
			break;
		case R_FV2:
			uni_printf(io, "$f2");
			break;
		case R_FV3:
			uni_printf(io, "$f3");
			break;

		case R_FT0:
			uni_printf(io, "$f4");
			break;
		case R_FT1:
			uni_printf(io, "$f5");
			break;
		case R_FT2:
			uni_printf(io, "$f6");
			break;
		case R_FT3:
			uni_printf(io, "$f7");
			break;
		case R_FT4:
			uni_printf(io, "$f8");
			break;
		case R_FT5:
			uni_printf(io, "$f9");
			break;
		case R_FT6:
			uni_printf(io, "$f10");
			break;
		case R_FT7:
			uni_printf(io, "$f11");
			break;
		case R_FT8:
			uni_printf(io, "$f16");
			break;
		case R_FT9:
			uni_printf(io, "$f17");
			break;
		case R_FT10:
			uni_printf(io, "$f18");
			break;
		case R_FT11:
			uni_printf(io, "$f19");
			break;

		case R_FA0:
			uni_printf(io, "$f12");
			break;
		case R_FA1:
			uni_printf(io, "$f13");
			break;
		case R_FA2:
			uni_printf(io, "$f14");
			break;
		case R_FA3:
			uni_printf(io, "$f15");
			break;

		case R_FS0:
			uni_printf(io, "$f20");
			break;
		case R_FS1:
			uni_printf(io, "$f21");
			break;
		case R_FS2:
			uni_printf(io, "$f22");
			break;
		case R_FS3:
			uni_printf(io, "$f23");
			break;
		case R_FS4:
			uni_printf(io, "$f24");
			break;
		case R_FS5:
			uni_printf(io, "$f25");
			break;
		case R_FS6:
			uni_printf(io, "$f26");
			break;
		case R_FS7:
			uni_printf(io, "$f27");
			break;
		case R_FS8:
			uni_printf(io, "$f28");
			break;
		case R_FS9:
			uni_printf(io, "$f29");
			break;
		case R_FS10:
			uni_printf(io, "$f30");
			break;
		case R_FS11:
			uni_printf(io, "$f31");
			break;
	}
}

static void instruction_to_io(universal_io *const io, const mips_instruction_t instruction)
{
	switch (instruction)
	{
		case IC_MIPS_MOVE:
			uni_printf(io, "move");
			break;
		case IC_MIPS_LI:
			uni_printf(io, "li");
			break;
		case IC_MIPS_LA:
			uni_printf(io, "la");
			break;
		case IC_MIPS_NOT:
			uni_printf(io, "not");
			break;

		case IC_MIPS_ADDI:
			uni_printf(io, "addi");
			break;
		case IC_MIPS_SLL:
			uni_printf(io, "sll");
			break;
		case IC_MIPS_SRA:
			uni_printf(io, "sra");
			break;
		case IC_MIPS_ANDI:
			uni_printf(io, "andi");
			break;
		case IC_MIPS_XORI:
			uni_printf(io, "xori");
			break;
		case IC_MIPS_ORI:
			uni_printf(io, "ori");
			break;

		case IC_MIPS_ADD:
			uni_printf(io, "add");
			break;
		case IC_MIPS_SUB:
			uni_printf(io, "sub");
			break;
		case IC_MIPS_MUL:
			uni_printf(io, "mul");
			break;
		case IC_MIPS_DIV:
			uni_printf(io, "div");
			break;
		case IC_MIPS_MOD:
			uni_printf(io, "mod");
			break;
		case IC_MIPS_SLLV:
			uni_printf(io, "sllv");
			break;
		case IC_MIPS_SRAV:
			uni_printf(io, "srav");
			break;
		case IC_MIPS_AND:
			uni_printf(io, "and");
			break;
		case IC_MIPS_XOR:
			uni_printf(io, "xor");
			break;
		case IC_MIPS_OR:
			uni_printf(io, "or");
			break;

		case IC_MIPS_SW:
			uni_printf(io, "sw");
			break;
		case IC_MIPS_LW:
			uni_printf(io, "lw");
			break;

		case IC_MIPS_JR:
			uni_printf(io, "jr");
			break;
		case IC_MIPS_JAL:
			uni_printf(io, "jal");
			break;
		case IC_MIPS_J:
			uni_printf(io, "j");
			break;

		case IC_MIPS_BLEZ:
			uni_printf(io, "blez");
			break;
		case IC_MIPS_BLTZ:
			uni_printf(io, "bltz");
			break;
		case IC_MIPS_BGEZ:
			uni_printf(io, "bgez");
			break;
		case IC_MIPS_BGTZ:
			uni_printf(io, "bgtz");
			break;
		case IC_MIPS_BEQ:
			uni_printf(io, "beq");
			break;
		case IC_MIPS_BNE:
			uni_printf(io, "bne");
			break;

		case IC_MIPS_NOP:
			uni_printf(io, "nop");
			break;

		case IC_MIPS_ADD_S:
			uni_printf(io, "add.s");
			break;
		case IC_MIPS_SUB_S:
			uni_printf(io, "sub.s");
			break;
		case IC_MIPS_MUL_S:
			uni_printf(io, "mul.s");
			break;
		case IC_MIPS_DIV_S:
			uni_printf(io, "div.s");
			break;

		case IC_MIPS_S_S:
			uni_printf(io, "s.s");
			break;
		case IC_MIPS_L_S:
			uni_printf(io, "l.s");
			break;

		case IC_MIPS_LI_S:
			uni_printf(io, "li.s");
			break;

		case IC_MIPS_MOV_S:
			uni_printf(io, "mov.s");
			break;

		case IC_MIPS_MFC_1:
			uni_printf(io, "mfc1");
			break;
		case IC_MIPS_MFHC_1:
			uni_printf(io, "mfhc1");
			break;

		case IC_MIPS_CVT_D_S:
			uni_printf(io, "cvt.d.s");
			break;
		case IC_MIPS_CVT_S_W:
			uni_printf(io, "cvt.s.w");
			break;
		case IC_MIPS_CVT_W_S:
			uni_printf(io, "cvt.w.s");
			break;
	}
}

// Вид инструкции:	instr	fst_reg, snd_reg
static void to_code_2R(universal_io *const io, const mips_instruction_t instruction
	, const mips_register_t fst_reg, const mips_register_t snd_reg)
{
	uni_printf(io, "\t");
	instruction_to_io(io, instruction);
	uni_printf(io, " ");
	mips_register_to_io(io, fst_reg);
	uni_printf(io, ", ");
	mips_register_to_io(io, snd_reg);
	uni_printf(io, "\n");
}

// Вид инструкции:	instr	fst_reg, snd_reg, imm
static void to_code_2R_I(universal_io *const io, const mips_instruction_t instruction
	, const mips_register_t fst_reg, const mips_register_t snd_reg, const item_t imm)
{
	uni_printf(io, "\t");
	instruction_to_io(io, instruction);
	uni_printf(io, " ");
	mips_register_to_io(io, fst_reg);
	uni_printf(io, ", ");
	mips_register_to_io(io, snd_reg);
	uni_printf(io, ", %" PRIitem "\n", imm);
}

// Вид инструкции:	instr	fst_reg, imm(snd_reg)
static void to_code_R_I_R(universal_io *const io, const mips_instruction_t instruction
	, const mips_register_t fst_reg, const item_t imm, const mips_register_t snd_reg)
{
	uni_printf(io, "\t");
	instruction_to_io(io, instruction);
	uni_printf(io, " ");
	mips_register_to_io(io, fst_reg);
	uni_printf(io, ", %" PRIitem "(", imm);
	mips_register_to_io(io, snd_reg);
	uni_printf(io, ")\n");
}

// Вид инструкции:	instr	reg, imm
static void to_code_R_I(universal_io *const io, const mips_instruction_t instruction
	, const mips_register_t reg, const item_t imm)
{
	uni_printf(io, "\t");
	instruction_to_io(io, instruction);
	uni_printf(io, " ");
	mips_register_to_io(io, reg);
	uni_printf(io, ", %" PRIitem "\n", imm);
}

// Вид инструкции:	instr	reg
static void to_code_R(universal_io *const io, const mips_instruction_t instruction
	, const mips_register_t reg)
{
	uni_printf(io, "\t");
	instruction_to_io(io, instruction);
	uni_printf(io, " ");
	mips_register_to_io(io, reg);
	uni_printf(io, "\n");
}

/**
 * Writes "val" field of rvalue structure to io
 *
 * @param	io			Universal i/o (?)
 * @param	rval		Rvalue whose value is to be printed
 */
static void rvalue_const_to_io(universal_io *const io, const rvalue rval)
{
	// TODO: Оставшиеся типы
	switch (rval.type)
	{
		case TYPE_BOOLEAN:
		case TYPE_CHARACTER:
		case TYPE_INTEGER:
			uni_printf(io, "%" PRIitem, rval.val.int_val);
			break;

		case TYPE_FLOATING:
			uni_printf(io, "%f", rval.val.float_val);

		default:
			break;
	}
}

/**
 * Writes rvalue to io
 * 
 * @param	info			Codegen info (?)
 * @param	rval			Rvalue to write
*/
static void rvalue_to_io(information *const info, const rvalue rval)
{
	assert(rval.kind != RVALUE_KIND_VOID);

	if (rval.kind == RVALUE_KIND_CONST)
	{
		rvalue_const_to_io(info->sx->io, rval);
	}
	else
	{
		mips_register_to_io(info->sx->io, rval.val.reg_num);
	}
}

/**
 * Add new identifier to displacements table
 * 
 * @param	info			Codegen info (?)
 * @param	identifier		Identifier for adding to the table
 * 
 * @return	Identifier lvalue
 */
static lvalue displacements_add(information *const info, const size_t identifier)
{
	const size_t displacement = info->max_displ;
	const mips_register_t base_reg = ident_is_local(info->sx, identifier) ? R_SP : R_GP;
	const item_t type = ident_get_type(info->sx, identifier);

	const size_t index = hash_add(&info->displacements, identifier, 3);
	hash_set_by_index(&info->displacements, index, 0, LVALUE_KIND_STACK); // TODO: регистровые переменные
	hash_set_by_index(&info->displacements, index, 1, displacement);
	hash_set_by_index(&info->displacements, index, 2, base_reg);

	return (lvalue){ .kind = LVALUE_KIND_STACK, .base_reg = base_reg, .loc.displ = displacement, .type = type };
}

/**
 * Return lvalue for the given identifier
 * 
 * @param	info			Codegen info (?)
 * @param	identifier		Identifier in the table
 * 
 * @return	Identifier lvalue
 */
static lvalue displacements_get(information *const info, const size_t identifier)
{
	const lvalue_kind_t kind = hash_get(&info->displacements, identifier, 0);
	const size_t displacement = hash_get(&info->displacements, identifier, 1);
	const mips_register_t base_reg = hash_get(&info->displacements, identifier, 2);
	const item_t type = ident_get_type(info->sx, identifier);

	return (lvalue){ .kind = kind, .base_reg = base_reg, .loc.displ = displacement, .type = type };
}

/**
 * Emit label
 *
 * @param	info			Codegen info (?)
 * @param	label			Label for emitting
 */
static void emit_label(information *const info, const label lbl)
{
	universal_io *const io = info->sx->io;
	switch (lbl.kind)
	{
		case L_FUNC:
			uni_printf(io, "FUNC");
			break;
		case L_NEXT:
			uni_printf(io, "NEXT");
			break;
		case L_FUNCEND:
			uni_printf(io, "FUNCEND");
			break;
		case L_STRING:
			uni_printf(io, "STRING");
			break;
		case L_ELSE:
			uni_printf(io, "ELSE");
			break;
		case L_END:
			uni_printf(io, "END");
			break;
		case L_BEGIN_CYCLE:
			uni_printf(io, "BEGIN_CYCLE");
			break;
	}

	uni_printf(io, "%" PRIitem, lbl.num);
}

/**
 * Emit label declaration
 *
 * @param	info			Codegen info (?)
 * @param	label			Declared label
 */
static void emit_label_declaration(information *const info, const label lbl)
{
	emit_label(info, lbl);
	uni_printf(info->sx->io, ":\n");
}

/**
 * Emit unconditional branch
 *
 * @param	info			Codegen info (?)
 * @param	label			Label for unconditional jump
 */
static void emit_unconditional_branch(information *const info, const label lbl)
{
	uni_printf(info->sx->io, "\t");
	instruction_to_io(info->sx->io, IC_MIPS_J);
	uni_printf(info->sx->io, " ");
	emit_label(info, lbl);
	uni_printf(info->sx->io, "\n");
}

/**
 * Emit conditional branch
 *
 * @param	info			Codegen info (?)
 * @param	label			Label for conditional jump
 */
static void emit_conditional_branch(information *const info, const rvalue value, const label lbl)
{
	if (value.kind == RVALUE_KIND_CONST)
	{
		bool is_zero;
		if (type_is_floating(type_get_class(info->sx, value.type)))
		{
			is_zero = value.val.float_val == 0.0;
		}
		else
		{
			is_zero = value.val.int_val == 0;
		}

		if (is_zero)
		{
			emit_unconditional_branch(info, lbl);
		}
	}
	else
	{
		uni_printf(info->sx->io, "\t");
		instruction_to_io(info->sx->io, IC_MIPS_BEQ);
		uni_printf(info->sx->io, " ");
		rvalue_to_io(info, value);
		uni_printf(info->sx->io, ", $0, ");
		emit_label(info, lbl);
		uni_printf(info->sx->io, "\n");
	}
}


/*
 *	 ______     __  __     ______   ______     ______     ______     ______     __     ______     __   __     ______
 *	/\  ___\   /\_\_\_\   /\  == \ /\  == \   /\  ___\   /\  ___\   /\  ___\   /\ \   /\  __ \   /\ "-.\ \   /\  ___\
 *	\ \  __\   \/_/\_\/_  \ \  _-/ \ \  __<   \ \  __\   \ \___  \  \ \___  \  \ \ \  \ \ \/\ \  \ \ \-.  \  \ \___  \
 *	 \ \_____\   /\_\/\_\  \ \_\    \ \_\ \_\  \ \_____\  \/\_____\  \/\_____\  \ \_\  \ \_____\  \ \_\\"\_\  \/\_____\
 *	  \/_____/   \/_/\/_/   \/_/     \/_/ /_/   \/_____/   \/_____/   \/_____/   \/_/   \/_____/   \/_/ \/_/   \/_____/
 */


/**
 * Loads lvalue to register and forms rvalue
 *
 * @param	info			Codegen info (?)
 * @param	lval			Lvalue to load
 *
 * @return	Formed rvalue
 */
static rvalue emit_load_of_lvalue(information *const info, const lvalue lval)
{
	if (lval.kind == LVALUE_KIND_REGISTER)
	{
		return (rvalue) { 
			.kind = RVALUE_KIND_REGISTER, 
			.val.reg_num = lval.loc.reg_num, 
			.from_lvalue = FROM_LVALUE,
			.type = lval.type
		};
	}

	const bool is_floating = type_is_floating(lval.type);
	const mips_register_t reg = is_floating ? get_float_register(info) : get_register(info);
	const mips_instruction_t instruction = is_floating ? IC_MIPS_L_S : IC_MIPS_LW;

	const rvalue result = { 
		.kind = RVALUE_KIND_REGISTER,
		.val.reg_num = reg,
		.from_lvalue = !FROM_LVALUE,
		.type = lval.type,
	};

	uni_printf(info->sx->io, "\t");
	instruction_to_io(info->sx->io, IC_MIPS_L_S);
	uni_printf(info->sx->io, " ");
	rvalue_to_io(info, result);
	uni_printf(info->sx->io, ", %" PRIitem "(", lval.loc.displ);
	mips_register_to_io(info->sx->io, lval.base_reg);
	uni_printf(info->sx->io, ")\n");

	// Для любых скалярных типов ничего не произойдёт,
	// а для остальных освобождается base_reg, в котором хранилось смещение
	free_register(info, lval.base_reg);

	return result;
}

/**
 * Emit identifier lvalue
 *
 * @param	info			Codegen info (?)
 * @param	nd  			Node in AST
 *
 * @return	Identifier lvalue
 */
static lvalue emit_identifier_lvalue(information *const info, const node *const nd)
{
	return displacements_get(info, expression_identifier_get_id(nd));
}

/**
 * Emit subscript lvalue
 *
 * @param	info			Codegen info (?)
 * @param	nd				Node in AST
 *
 * @return	Subscript lvalue
 */
static lvalue emit_subscript_lvalue(information *const info, const node *const nd)
{
	const node base = expression_subscript_get_base(nd);
	const rvalue base_value = emit_expression(info, &base); 

	const node index = expression_subscript_get_index(nd);
	const rvalue index_value = emit_expression(info, &index);

	// base_value гарантированно имеет kind == RVALUE_KIND_REGISTER
	// TODO: константу можно сразу записывать в loc.displ
	const rvalue result = emit_binary_operation(info, base_value, index_value, BIN_ADD);

	const item_t type = expression_get_type(nd);
	return (lvalue) { .kind = LVALUE_KIND_STACK, .base_reg = result.val.reg_num, .loc.displ = 0, .type = type };
}

/**
 * Emit member lvalue
 *
 * @param	info			Codegen info (?)
 * @param	nd				Node in AST
 *
 * @return	Created lvalue
 */
static lvalue emit_member_lvalue(information *const info, const node *const nd)
{
	const node base = expression_member_get_base(nd);
	const item_t base_type = expression_get_type(&base);

	const bool is_arrow = expression_member_is_arrow(nd);
	const item_t struct_type = is_arrow ? type_pointer_get_element_type(info->sx, base_type) : base_type;

	size_t member_displ = 0;
	const size_t member_index = expression_member_get_member_index(nd);
	for (size_t i = 0; i < member_index; i++)
	{
		const item_t member_type = type_structure_get_member_type(info->sx, struct_type, i);
		member_displ += type_size(info->sx, member_type);
	}

	const item_t type = expression_get_type(nd);

	if (is_arrow)
	{
		const rvalue struct_pointer = emit_expression(info, &base);
		// FIXME: грузить константу на регистр в случае константных указателей
		return (lvalue) {
			.kind = LVALUE_KIND_STACK, 
			.base_reg = struct_pointer.val.reg_num,
			.loc.displ = member_displ, 
			.type = type
		};
	}
	else
	{
		const lvalue base_lvalue = emit_lvalue(info, &base);
		const size_t displ = base_lvalue.loc.displ + member_displ;
		return (lvalue) { 
			.kind = LVALUE_KIND_STACK,
			.base_reg = base_lvalue.base_reg, 
			.loc.displ = displ, 
			.type = type 
		};
	}
}

/**
 * Emit indirection lvalue
 * 
 * @param	info			Codegen info (?)
 * @param	nd				Node in AST
 * 
 * @return	Indirected lvalue
*/
static lvalue emit_indirection_lvalue(information *const info, const node *const nd)
{
	assert(expression_unary_get_operator(nd) == UN_INDIRECTION);

	const node operand = expression_unary_get_operand(nd);
	const rvalue base = emit_expression(info, &operand);
	// FIXME: грузить константу на регистр в случае константных указателей
	const item_t type = expression_get_type(nd);

	return (lvalue) {
		.kind = LVALUE_KIND_STACK,
		.base_reg = base.val.reg_num,
		.loc.displ = 0,
		.type = type
	};
}

/**
 * Emit lvalue expression
 *
 * @param	info			Information
 * @param	nd				Node in AST
 *
 * @return	Lvalue
 */
static lvalue emit_lvalue(information *const info, const node *const nd)
{
	// TODO: ассерты
	switch (expression_get_class(nd))
	{
		case EXPR_IDENTIFIER:
			return emit_identifier_lvalue(info, nd);

		case EXPR_SUBSCRIPT:
			return emit_subscript_lvalue(info, nd);

		case EXPR_MEMBER:
			return emit_member_lvalue(info, nd);

		case EXPR_UNARY: // Только UN_INDIRECTION
			return emit_indirection_lvalue(info, nd);

		default:
			// Не может быть lvalue
			system_error(node_unexpected, nd);
			return (lvalue){ .loc.displ = ITEM_MAX };
	}
}


/**
 * Stores rvalue to lvalue
 *
 * @param	info			Codegen info (?)
 * @param	rval			Rvalue to store
 * @param	lval			Lvalue to store rvalue to
 *
 * @return	Formed lvalue
 */
static lvalue emit_store_of_rvalue(information *const info, rvalue rval, const lvalue lval)
{
	assert(rval.kind != RVALUE_KIND_VOID);

	if (rval.kind == RVALUE_KIND_CONST)
	{
		// Предварительно загружаем константу в rvalue вида RVALUE_KIND_REGISTER
		const rvalue tmp_rval = rval;
		rval = (rvalue){ .kind = RVALUE_KIND_REGISTER,
						 .val.reg_num = type_is_floating(tmp_rval.type) ? get_float_register(info) : get_register(info),
						 .type = tmp_rval.type,
						 .from_lvalue = !FROM_LVALUE };
		emit_store_rvalue_to_rvalue(info, rval, tmp_rval);
	}

	uni_printf(info->sx->io, "\t");

	if (lval.kind == LVALUE_KIND_REGISTER)
	{
		instruction_to_io(info->sx->io, type_is_floating(rval.type) ? IC_MIPS_MOV_S : IC_MIPS_MOVE);
		uni_printf(info->sx->io, " ");
		rvalue_to_io(info, rval);
		uni_printf(info->sx->io, ", ");
		rvalue_to_io(info, emit_load_of_lvalue(info, lval));
		uni_printf(info->sx->io, "\n");
	}
	else
	{
		if ((!type_is_structure(info->sx, type_get_class(info->sx, lval.type))) &&
			(!type_is_array(info->sx, type_get_class(info->sx, lval.type))))
		{
			instruction_to_io(info->sx->io, type_is_floating(rval.type) ? IC_MIPS_S_S : IC_MIPS_SW);
			uni_printf(info->sx->io, " ");
			rvalue_to_io(info, rval);
			uni_printf(info->sx->io, ", %" PRIitem "(", lval.loc.displ);
			mips_register_to_io(info->sx->io, lval.base_reg);
			uni_printf(info->sx->io, ")\n");
		}
		else
		{
			// Делаем store для каждого отдельного элемента переданного lval
			const size_t amount = type_structure_get_member_amount(info->sx, lval.type);
			for (size_t i = 0; i < amount; i++)
			{
				const item_t member_type = type_structure_get_member_type(info->sx, lval.type, i);
				size_t member_size = WORD_LENGTH * type_size(info->sx, member_type);
				// FIXME: type_size для floating вернёт 2, но у нас single precision => под них нужно 1
				if (type_is_floating(member_type))
				{
					member_size -= WORD_LENGTH;
				}
				const rvalue displ_rval = emit_load_of_lvalue(
					info, (lvalue){ .loc.displ = i * member_size,
									.base_reg = rval.val.reg_num // Здесь смещение, по которому лежит массив/структура
									,
									.type = member_type,
									.kind = LVALUE_KIND_STACK });

				emit_store_of_rvalue(info, displ_rval,
									 (lvalue){ .base_reg = lval.base_reg,
											   .kind = LVALUE_KIND_STACK,
											   .loc.displ = lval.loc.displ + i * member_size,
											   .type = member_type });

				free_rvalue(info, displ_rval);
			}
		}
	}

	return lval;
}

/**
 * Stores rvalue into another rvalue (register-kind)
 *
 * @param info				Codegen info (?)
 * @param destination		Rvalue where "source" parameter will be stored
 * @param source			Rvalue to store
 */
static void emit_store_rvalue_to_rvalue(information *const info, const rvalue destination, const rvalue source)
{
	assert(source.kind != RVALUE_KIND_VOID);
	assert(destination.kind == RVALUE_KIND_REGISTER);

	if (source.kind == RVALUE_KIND_CONST)
	{
		uni_printf(info->sx->io, "\t");
		(type_is_floating(source.type)) ? instruction_to_io(info->sx->io, IC_MIPS_LI_S)
										: instruction_to_io(info->sx->io, IC_MIPS_LI);
		uni_printf(info->sx->io, " ");
		rvalue_to_io(info, destination);
		uni_printf(info->sx->io, ", ");
		rvalue_to_io(info, source);
		uni_printf(info->sx->io, "\n");
	}
	else
	{
		if (destination.val.reg_num == source.val.reg_num)
		{
			uni_printf(info->sx->io, "\t# stays in register");
			mips_register_to_io(info->sx->io, destination.val.reg_num);
			uni_printf(info->sx->io, "\n");
		}
		else
		{
			switch (type_get_class(info->sx, source.type))
			{
				case TYPE_BOOLEAN:
				case TYPE_CHARACTER:
				case TYPE_INTEGER:
				case TYPE_ARRAY: // в этом случае в регистре просто смещение в динамике
					uni_printf(info->sx->io, "\t");
					instruction_to_io(info->sx->io, IC_MIPS_MOVE);
					uni_printf(info->sx->io, " ");
					rvalue_to_io(info, destination);
					uni_printf(info->sx->io, ", ");
					rvalue_to_io(info, source);
					uni_printf(info->sx->io, "\n");
					break;

				case TYPE_FLOATING:
					uni_printf(info->sx->io, "\t");
					instruction_to_io(info->sx->io, IC_MIPS_MOV_S);
					uni_printf(info->sx->io, " ");
					rvalue_to_io(info, destination);
					uni_printf(info->sx->io, ", ");
					rvalue_to_io(info, source);
					uni_printf(info->sx->io, "\n");
					break;

				default: // TYPE_STRUCTURE
					break;
			}
		}
	}
}

/**
 * Emit binary operation with two rvalues
 *
 * @param	info			Codegen info (?)
 * @param	first_rval1		First rvalue
 * @param	second_rval2	Second rvalue
 * @param	operation		Operation
 *
 * @return	Result rvalue
 */
static rvalue emit_binary_operation(information *const info, rvalue rval1, rvalue rval2, const binary_t operation)
{
	assert(operation != BIN_LOG_AND);
	assert(operation != BIN_LOG_OR);

	assert(rval1.kind != RVALUE_KIND_VOID);
	assert(rval2.kind != RVALUE_KIND_VOID);

	mips_register_t result;
	rvalue result_rvalue;
	rvalue freeing_rvalue = (rvalue){ .kind = RVALUE_KIND_VOID };

	if ((rval1.kind == RVALUE_KIND_REGISTER) && (rval2.kind == RVALUE_KIND_REGISTER))
	{
		if (!rval1.from_lvalue && !rval2.from_lvalue) // Оба rvalue -- не регистровые переменные
		{
			// Возьмём тогда для результата минимальный регистр, а другой впоследствии будет отброшен
			if (rval1.val.reg_num > rval2.val.reg_num)
			{
				result = rval2.val.reg_num;
				freeing_rvalue = rval1;
			}
			else
			{
				result = rval1.val.reg_num;
				freeing_rvalue = rval2;
			}
		} // В противном случае никакой регистр освобождать не требуется, т.к. в нём будет записан результат
		else if ((rval1.from_lvalue) && (!rval2.from_lvalue))
		{
			result = rval2.val.reg_num;
		}
		else if ((rval2.from_lvalue) && (!rval1.from_lvalue))
		{
			result = rval1.val.reg_num;
		}
		else
		{
			result = type_is_floating(rval1.type) ? get_float_register(info) : get_register(info);
		}

		result_rvalue = (rvalue){ .kind = RVALUE_KIND_REGISTER
			, .val.reg_num = result
			, .type = rval1.type
			, .from_lvalue = !FROM_LVALUE };

		switch (operation)
		{
			case BIN_LT:
			case BIN_GT:
			case BIN_LE:
			case BIN_GE:
			case BIN_EQ:
			case BIN_NE:
				const item_t curr_label_num = info->label_num++;
				const label label_else = { .kind = L_ELSE, .num = curr_label_num };
				const label label_end = { .kind = L_END, .num = curr_label_num };

				uni_printf(info->sx->io, "\t");
				instruction_to_io(info->sx->io, IC_MIPS_SUB);
				uni_printf(info->sx->io, " ");
				rvalue_to_io(info, result_rvalue);
				uni_printf(info->sx->io, ", ");
				rvalue_to_io(info, rval1);
				uni_printf(info->sx->io, ", ");
				rvalue_to_io(info, rval2);
				uni_printf(info->sx->io, "\n");


				uni_printf(info->sx->io, "\t");
				if (operation == BIN_GT)
				{
					instruction_to_io(info->sx->io, IC_MIPS_BGTZ);
				}
				else if (operation == BIN_LT)
				{
					instruction_to_io(info->sx->io, IC_MIPS_BLTZ);
				}
				else if (operation == BIN_GE)
				{
					instruction_to_io(info->sx->io, IC_MIPS_BGEZ);
				}
				else if (operation == BIN_LE)
				{
					instruction_to_io(info->sx->io, IC_MIPS_BLEZ);
				}
				else if (operation == BIN_EQ)
				{
					instruction_to_io(info->sx->io, IC_MIPS_BEQ);
				}
				else // BIN_NE
				{
					instruction_to_io(info->sx->io, IC_MIPS_BNE);
				}
				uni_printf(info->sx->io, " ");
				rvalue_to_io(info, result_rvalue);
				uni_printf(info->sx->io, ", ");
				emit_label(info, label_else);

				uni_printf(info->sx->io, "\t");
				instruction_to_io(info->sx->io, IC_MIPS_LI);
				uni_printf(info->sx->io, " ");
				rvalue_to_io(info, result_rvalue);
				uni_printf(info->sx->io, ", 1\n");
				emit_label(info, label_end);

				emit_label(info, label_else);
				uni_printf(info->sx->io, "\t");
				instruction_to_io(info->sx->io, IC_MIPS_LI);
				uni_printf(info->sx->io, " ");
				rvalue_to_io(info, result_rvalue);
				uni_printf(info->sx->io, ", 0\n");

				emit_label_declaration(info, label_end);

				uni_printf(info->sx->io, "\n");
				break;

			default:
				uni_printf(info->sx->io, "\t");
				instruction_to_io(info->sx->io,
								  get_bin_instruction(operation, /* Два регистра => 0 в get_bin_instruction() -> */ 0));
				uni_printf(info->sx->io, " ");
				rvalue_to_io(info, result_rvalue);
				uni_printf(info->sx->io, ", ");
				rvalue_to_io(info, rval1);
				uni_printf(info->sx->io, ", ");
				rvalue_to_io(info, rval2);
				uni_printf(info->sx->io, "\n");
		}
	}
	else
	{
		// Гарантируется, что будет ровно один оператор в регистре и один оператор в константе

		// Сделаем так, чтобы константа гарантированно лежала в rval2
		if (rval2.kind != RVALUE_KIND_CONST)
		{
			const rvalue tmp_rval = rval1;
			rval1 = rval2;
			rval2 = tmp_rval;
		}

		if (rval1.from_lvalue)
		{
			result = type_is_floating(rval1.type) ? get_float_register(info) : get_register(info);
		}
		else
		{
			result = rval1.val.reg_num;
		}

		result_rvalue =
			(rvalue){ .kind = RVALUE_KIND_REGISTER, .val.reg_num = result, .type = rval1.type, .from_lvalue = !FROM_LVALUE };

		switch (operation)
		{
			case BIN_LT:
			case BIN_GT:
			case BIN_LE:
			case BIN_GE:
			case BIN_EQ:
			case BIN_NE:
			{
				const item_t curr_label_num = info->label_num++;
				const label label_else = { .kind = L_ELSE, .num = curr_label_num };
				const label label_end = { .kind = L_END, .num = curr_label_num };

				// TODO: Оптимизации с умножением на (-1)
				// Загружаем <значение из rval2> на регистр
				const rvalue tmp_rval = rval2;
				rval2 = (rvalue){ .kind = RVALUE_KIND_REGISTER,
								  .val.reg_num =
									  type_is_floating(tmp_rval.type) ? get_float_register(info) : get_register(info),
								  .type = tmp_rval.type,
								  .from_lvalue = !FROM_LVALUE };
				emit_store_rvalue_to_rvalue(info, rval2, tmp_rval);

				// Записываем <значение из rval1> - <значение из rval2> в result
				uni_printf(info->sx->io, "\t");
				instruction_to_io(info->sx->io, IC_MIPS_SUB);
				uni_printf(info->sx->io, " ");
				rvalue_to_io(info, result_rvalue);
				uni_printf(info->sx->io, ", ");
				rvalue_to_io(info, rval1);
				uni_printf(info->sx->io, ", ");
				mips_register_to_io(info->sx->io, rval2.val.reg_num);
				uni_printf(info->sx->io, "\n");

				uni_printf(info->sx->io, "\t");
				if (operation == BIN_GT)
				{
					instruction_to_io(info->sx->io, IC_MIPS_BGTZ);
				}
				else if (operation == BIN_LT)
				{
					instruction_to_io(info->sx->io, IC_MIPS_BLTZ);
				}
				else if (operation == BIN_GE)
				{
					instruction_to_io(info->sx->io, IC_MIPS_BGEZ);
				}
				else if (operation == BIN_LE)
				{
					instruction_to_io(info->sx->io, IC_MIPS_BLEZ);
				}
				else if (operation == BIN_EQ)
				{
					instruction_to_io(info->sx->io, IC_MIPS_BEQ);
				}
				else // BIN_NE
				{
					instruction_to_io(info->sx->io, IC_MIPS_BNE);
				}
				uni_printf(info->sx->io, " ");
				rvalue_to_io(info, result_rvalue);
				uni_printf(info->sx->io, ", ");
				emit_label(info, label_else);

				uni_printf(info->sx->io, "\t");
				instruction_to_io(info->sx->io, IC_MIPS_LI);
				uni_printf(info->sx->io, " ");
				rvalue_to_io(info, result_rvalue);
				uni_printf(info->sx->io, ", 1\n");
				emit_unconditional_branch(info, label_end);

				emit_label(info, label_else);
				uni_printf(info->sx->io, "\t");
				instruction_to_io(info->sx->io, IC_MIPS_LI);
				uni_printf(info->sx->io, " ");
				rvalue_to_io(info, result_rvalue);
				uni_printf(info->sx->io, ", 0\n");

				emit_label_declaration(info, label_end);

				uni_printf(info->sx->io, "\n");
				break;
			}

			default:
				// TODO: Оптимизации
				// Предварительно загружаем константу из rval2 в rvalue вида RVALUE_KIND_REGISTER
				if ((operation == BIN_SUB) || (operation == BIN_DIV) || (operation == BIN_MUL) ||
					(operation == BIN_REM) || (operation == BIN_DIV))
				{
					const rvalue tmp_rval = rval2;
					rval2 = (rvalue){ .kind = RVALUE_KIND_REGISTER,
									  .val.reg_num = type_is_floating(tmp_rval.type) ? get_float_register(info)
																					 : get_register(info),
									  .type = tmp_rval.type,
									  .from_lvalue = !FROM_LVALUE };
					emit_store_rvalue_to_rvalue(info, rval2, tmp_rval);
				}
				// Нет команд вычитания из значения по регистру константы, так что умножаем на (-1)
				if (operation == BIN_SUB)
				{
					emit_binary_operation(info, rval2, rvalue_negative_one, BIN_MUL);
				}

				// Выписываем операцию, её результат будет записан в result
				uni_printf(info->sx->io, "\t");
				instruction_to_io(info->sx->io
								  // FIXME:
								  ,
								  get_bin_instruction(operation, /* Один регистр => 1 в get_bin_instruction() -> */ 1));
				uni_printf(info->sx->io, " ");
				rvalue_to_io(info, result_rvalue);
				uni_printf(info->sx->io, ", ");
				rvalue_to_io(info, rval1);
				uni_printf(info->sx->io, ", ");
				rvalue_to_io(info, rval2);
				uni_printf(info->sx->io, "\n");

				free_rvalue(info, rval2);
		}
	}

	free_rvalue(info, freeing_rvalue);

	return result_rvalue;
}

/**
 * Emit cast expression
 *
 * @param	info			Codegen info (?)
 * @param	nd				Node in AST
 *
 * @return	Rvalue of cast expression
 */
static rvalue emit_cast_expression(information *const info, const node *const nd)
{
	const node operand = expression_cast_get_operand(nd);
	rvalue operand_rval = emit_expression(info, &operand);

	if (type_get_class(info->sx, expression_get_type(nd)))
	{
		// char -> int
		// Ведёт себя ровно так же, как и целочисленный тип
		operand_rval.type = TYPE_INTEGER;
		return operand_rval;
	}
	else
	{
		// int -> float

		const rvalue result = (rvalue){ .from_lvalue = !FROM_LVALUE
			, .kind = RVALUE_KIND_REGISTER
			, .type = TYPE_FLOATING
			, .val.reg_num = get_float_register(info) };

		// FIXME: избавится от to_code функций
		to_code_2R(info->sx->io, IC_MIPS_MFC_1, operand_rval.val.reg_num, result.val.reg_num);
		to_code_2R(info->sx->io, IC_MIPS_CVT_S_W, result.val.reg_num, result.val.reg_num);

		free_rvalue(info, operand_rval);

		return result;
	}
}

/**
 *	Emit literal expression
 *
 * @param	info			Encoder
 * @param	nd				Node in AST
 *
 * @return	Rvalue of literal expression
 */
static rvalue emit_literal_expression(const syntax *const sx, const node *const nd)
{
	// Константа => хотим просто запомнить её значение

	const item_t type = expression_get_type(nd);
	switch (type_get_class(sx, type))
	{
		case TYPE_BOOLEAN:
			return (rvalue) { .kind = RVALUE_KIND_CONST
				, .type = type
				, .val.int_val = (expression_literal_get_boolean(nd)) ? 1 : 0
				, .from_lvalue = !FROM_LVALUE };

		case TYPE_CHARACTER:
			return (rvalue) { .kind = RVALUE_KIND_CONST
				, .type = type
				, .val.int_val = expression_literal_get_character(nd)
				, .from_lvalue = !FROM_LVALUE };

		case TYPE_INTEGER:
			return (rvalue) { .kind = RVALUE_KIND_CONST,
							 .type = type,
							 .val.int_val = expression_literal_get_integer(nd),
							 .from_lvalue = !FROM_LVALUE };

		case TYPE_FLOATING:
			return (rvalue) { .kind = RVALUE_KIND_CONST
				, .type = type
				, .val.float_val = expression_literal_get_floating(nd)
				, .from_lvalue = !FROM_LVALUE };

		case TYPE_ARRAY: // Только строка
			return (rvalue) { .kind = RVALUE_KIND_CONST
				, .type = type
				, .val.str_index = expression_literal_get_string(nd)
				, .from_lvalue = !FROM_LVALUE };

		default:
			return (rvalue){ .kind = RVALUE_KIND_VOID };
	}
}


/**
 * Emit printf expression
 *
 * @param	info				Codegen info (?)
 * @param	nd					AST node
 * @param	parameters_amount	Number of function parameters
 */
static void emit_printf_expression(information *const info, const node *const nd, const size_t parameters_amount)
{
	const node string = expression_call_get_argument(nd, 0);
	const size_t index = expression_literal_get_string(&string);
	const size_t amount = strings_amount(info->sx);

	for (size_t i = 1; i < parameters_amount; i++)
	{
		const node arg = expression_call_get_argument(nd, i);
		rvalue arg_rvalue = emit_expression(info, &arg);
		const item_t arg_rvalue_type = arg_rvalue.type;

		if (arg_rvalue.kind == RVALUE_KIND_CONST)
		{
			// Предварительно загружаем константу в rvalue вида RVALUE_KIND_REGISTER
			const rvalue tmp_rval = arg_rvalue;
			arg_rvalue = (rvalue){ .kind = RVALUE_KIND_REGISTER,
								   .val.reg_num =
									   type_is_floating(tmp_rval.type) ? get_float_register(info) : get_register(info),
								   .type = tmp_rval.type,
								   .from_lvalue = !FROM_LVALUE };
			emit_store_rvalue_to_rvalue(info, arg_rvalue, tmp_rval);
		}

		// Всегда хотим сохранять $a0 и $a1
		to_code_2R_I(info->sx->io, IC_MIPS_ADDI, R_FP, R_FP,
					 -(item_t)WORD_LENGTH *
						 (!type_is_floating(arg_rvalue_type) ? /* $a0 и $a1 */ 1 : /* $a0, $a1 и $a2 */ 2));
		uni_printf(info->sx->io, "\n");

		const lvalue a0_lval =
			emit_store_of_rvalue(info,
								 (rvalue){ .kind = RVALUE_KIND_REGISTER,
										   .val.reg_num = R_A0,
										   .type = TYPE_INTEGER // Не уверен
										   ,
										   .from_lvalue = !FROM_LVALUE },
								 (lvalue){ .base_reg = R_FP
										   // по call convention: первый на WORD_LENGTH выше предыдущего положения $fp,
										   // второй на 2*WORD_LENGTH и т.д.
										   ,
										   .loc.displ = 0,
										   .kind = LVALUE_KIND_STACK,
										   .type = arg_rvalue.type });

		const lvalue a1_lval =
			emit_store_of_rvalue(info,
								 (rvalue){ .kind = RVALUE_KIND_REGISTER,
										   .val.reg_num = R_A1,
										   .type = TYPE_INTEGER // Не уверен
										   ,
										   .from_lvalue = !FROM_LVALUE },
								 (lvalue){ .base_reg = R_FP
										   // по call convention: первый на WORD_LENGTH выше предыдущего положения $fp,
										   // второй на 2*WORD_LENGTH и т.д.
										   ,
										   .loc.displ = WORD_LENGTH,
										   .kind = LVALUE_KIND_STACK,
										   .type = arg_rvalue.type });

		if (!type_is_floating(arg_rvalue.type))
		{
			uni_printf(info->sx->io, "\n");
			emit_store_rvalue_to_rvalue(info
				, (rvalue) { .type = TYPE_INTEGER
					, .val.reg_num = R_A1
					, .kind = RVALUE_KIND_REGISTER
					, .from_lvalue = FROM_LVALUE }
				, arg_rvalue);

			uni_printf(info->sx->io, "\tlui $t1, %%hi(STRING%zu)\n", index + (i - 1) * amount);
			uni_printf(info->sx->io, "\taddiu $a0, $t1, %%lo(STRING%zu)\n", index + (i - 1) * amount);

			uni_printf(info->sx->io, "\tjal printf\n");
			uni_printf(info->sx->io, "\t");
			instruction_to_io(info->sx->io, IC_MIPS_NOP);
			uni_printf(info->sx->io, "\n");

			free_rvalue(info, arg_rvalue);

			uni_printf(info->sx->io, "\n\t# data restoring:\n");
		}
		else
		{
			const lvalue a2_lval = emit_store_of_rvalue(info
				, (rvalue) { .kind = RVALUE_KIND_REGISTER
					, .val.reg_num = R_A2
					, .type = TYPE_INTEGER
					, .from_lvalue = !FROM_LVALUE }
				, (lvalue) { .base_reg = R_FP
					// по call convention: первый на WORD_LENGTH выше предыдущего положения
					// $fp, второй на 2*WORD_LENGTH и т.д.
					, .loc.displ = 2 * WORD_LENGTH
					, .type = TYPE_INTEGER
					, .kind = LVALUE_KIND_STACK });
			uni_printf(info->sx->io, "\n");

			// Конвертируем single to double
			uni_printf(info->sx->io, "\t");
			instruction_to_io(info->sx->io, IC_MIPS_CVT_D_S);
			uni_printf(info->sx->io, " ");
			rvalue_to_io(info, arg_rvalue);
			uni_printf(info->sx->io, ", ");
			rvalue_to_io(info, arg_rvalue);
			uni_printf(info->sx->io, "\n");

			// Следующие действия необходимы, т.к. аргументы в builtin-функции обязаны передаваться в $a0-$a3
			// Даже для floating point!
			// %lo из arg_rvalue в $a1
			uni_printf(info->sx->io, "\t");
			instruction_to_io(info->sx->io, IC_MIPS_MFC_1);
			uni_printf(info->sx->io, " ");
			rvalue_to_io(info, (rvalue){ .from_lvalue = !FROM_LVALUE,
										 .kind = RVALUE_KIND_REGISTER,
										 .type = TYPE_INTEGER // Не уверен
										 ,
										 .val.reg_num = R_A1 });
			uni_printf(info->sx->io, ", ");
			rvalue_to_io(info, arg_rvalue);
			uni_printf(info->sx->io, "\n");

			// %hi из arg_rvalue в $a2
			uni_printf(info->sx->io, "\t");
			instruction_to_io(info->sx->io, IC_MIPS_MFHC_1);
			uni_printf(info->sx->io, " ");
			rvalue_to_io(info, (rvalue) { .from_lvalue = !FROM_LVALUE
				, .kind = RVALUE_KIND_REGISTER
				, .type = TYPE_INTEGER
				, .val.reg_num = R_A2 });
			uni_printf(info->sx->io, ", ");
			rvalue_to_io(info, arg_rvalue);
			uni_printf(info->sx->io, "\n");

			uni_printf(info->sx->io, "\tlui $t1, %%hi(STRING%zu)\n", index + (i - 1) * amount);
			uni_printf(info->sx->io, "\taddiu $a0, $t1, %%lo(STRING%zu)\n", index + (i - 1) * amount);

			uni_printf(info->sx->io, "\tjal printf\n");
			uni_printf(info->sx->io, "\t");
			instruction_to_io(info->sx->io, IC_MIPS_NOP);
			uni_printf(info->sx->io, "\n");

			// Восстановление регистров-аргументов -- они могут понадобится в дальнейшем
			uni_printf(info->sx->io, "\n\t# data restoring:\n");

			const rvalue a2_rval = emit_load_of_lvalue(info, a2_lval);
			emit_store_rvalue_to_rvalue(
				info, (rvalue){ .from_lvalue = FROM_LVALUE, .kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A2 }, a2_rval);
			free_rvalue(info, a2_rval);
			free_rvalue(info, arg_rvalue);
			uni_printf(info->sx->io, "\n");
		}

		const rvalue a0_rval = emit_load_of_lvalue(info, a0_lval);
		emit_store_rvalue_to_rvalue(info, (rvalue){ .from_lvalue = FROM_LVALUE, .kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A0 },
									a0_rval);
		free_rvalue(info, a0_rval);
		uni_printf(info->sx->io, "\n");

		const rvalue a1_rval = emit_load_of_lvalue(info, a1_lval);
		emit_store_rvalue_to_rvalue(info, (rvalue){ .from_lvalue = FROM_LVALUE, .kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A1 },
									a1_rval);
		free_rvalue(info, a1_rval);
		uni_printf(info->sx->io, "\n");

		to_code_2R_I(info->sx->io, IC_MIPS_ADDI, R_FP, R_FP,
					 (item_t)WORD_LENGTH *
						 (!type_is_floating(arg_rvalue_type) ? /* $a0 и $a1 */ 1 : /* $a0, $a1 и $a2 */ 2));
		uni_printf(info->sx->io, "\n");
	}

	const lvalue a0_lval = emit_store_of_rvalue(
		info, (rvalue){ .kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A0, .type = TYPE_INTEGER, .from_lvalue = !FROM_LVALUE },
		(lvalue){ .base_reg = R_FP
				  // по call convention: первый на WORD_LENGTH выше предыдущего положения $fp,
				  // второй на 2*WORD_LENGTH и т.д.
				  ,
				  .loc.displ = 0,
				  .kind = LVALUE_KIND_STACK,
				  .type = TYPE_INTEGER });

	uni_printf(info->sx->io, "\tlui $t1, %%hi(STRING%zu)\n", index + (parameters_amount - 1) * amount);
	uni_printf(info->sx->io, "\taddiu $a0, $t1, %%lo(STRING%zu)\n", index + (parameters_amount - 1) * amount);
	uni_printf(info->sx->io, "\tjal printf\n");
	uni_printf(info->sx->io, "\t");
	instruction_to_io(info->sx->io, IC_MIPS_NOP);
	uni_printf(info->sx->io, "\n");

	uni_printf(info->sx->io, "\n\t# data restoring:\n");
	const rvalue a0_rval = emit_load_of_lvalue(info, a0_lval);
	emit_store_rvalue_to_rvalue(info, (rvalue){ .from_lvalue = FROM_LVALUE, .kind = RVALUE_KIND_REGISTER, .val.reg_num = R_A0 },
								a0_rval);
	free_rvalue(info, a0_rval);

	// TODO: Возвращает число распечатанных символов (включая '\0'?)
}

/**
 * Emit call expression
 *
 * @param	info			Codegen info (?)
 * @param	nd				Node in AST
 *
 * @return	Rvalue of the result of call expression
 */
static rvalue emit_call_expression(information *const info, const node *const nd)
{
	const node callee = expression_call_get_callee(nd);
	size_t func_ref = expression_identifier_get_id(&callee);
	const size_t params_amount = expression_call_get_arguments_amount(nd);

	item_t return_type = type_function_get_return_type(info->sx, expression_get_type(&callee));

	uni_printf(info->sx->io, "\t# \"%s\" function call:\n", ident_get_spelling(info->sx, func_ref));

	if (func_ref == BI_PRINTF)
	{
		emit_printf_expression(info, nd, params_amount);
	}
	else
	{
		size_t f_arg_count = 0;
		size_t arg_count = 0;
		size_t displ_for_parameters = (params_amount - 1) * WORD_LENGTH;
		lvalue prev_arg_displ[TEMP_REG_AMOUNT /* за $a0-$a3 */
							  + TEMP_REG_AMOUNT / 2 /* за $fa0, $fa2 (т.к. single precision)*/];

		uni_printf(info->sx->io, "\t# setting up $fp:\n");
		if (displ_for_parameters)
		{
			to_code_2R_I(info->sx->io, IC_MIPS_ADDI, R_FP, R_FP, -(item_t)(displ_for_parameters));
		}

		uni_printf(info->sx->io, "\n\t# parameters passing:\n");

		// TODO: структуры/массивы

		size_t arg_reg_count = 0;
		for (size_t i = 0; i < params_amount; i++)
		{
			const node arg = expression_call_get_argument(nd, i);
			const rvalue arg_rvalue = emit_expression(info, &arg);

			if ((type_is_floating(arg_rvalue.type) ? f_arg_count : arg_count) < ARG_REG_AMOUNT)
			{
				uni_printf(info->sx->io, "\t# saving ");
				mips_register_to_io(info->sx->io, (type_is_floating(arg_rvalue.type) 
					? R_FA0 + f_arg_count 
					: R_A0 + arg_count));
				uni_printf(info->sx->io, " value on stack:\n");
			}
			else
			{
				uni_printf(info->sx->io, "\t# parameter on stack:\n");
			}

			const lvalue tmp_arg_lvalue = { .base_reg = R_FP
				// по call convention: первый на WORD_LENGTH выше предыдущего положения $fp,
				// второй на 2*WORD_LENGTH и т.д.
				, .loc.displ = i * WORD_LENGTH
				, .kind = LVALUE_KIND_STACK
				, .type = /* TODO: структуры/массивы здесь некорректно обработаются: */ arg_rvalue.type };

			// Сохранение текущего регистра-аргумента на стек либо передача аргументов на стек
			emit_store_of_rvalue(info
				, (type_is_floating(arg_rvalue.type) ? f_arg_count : arg_count) < ARG_REG_AMOUNT
					? (rvalue){ .kind = RVALUE_KIND_REGISTER
						, .val.reg_num = (type_is_floating(arg_rvalue.type) 
							? R_FA0 + f_arg_count 
							: R_A0 + arg_count)
						, .type = arg_rvalue.type
						, .from_lvalue = !FROM_LVALUE } // Сохранение значения в регистре-аргументе
					: arg_rvalue // Передача аргумента
				, tmp_arg_lvalue);

			// если это передача параметров в регистры-аргументы
			if ((type_is_floating(arg_rvalue.type) ? f_arg_count : arg_count) < ARG_REG_AMOUNT)
			{
				// Аргументы рассматриваются в данном случае как регистровые переменные
				const rvalue tmp = { .kind = RVALUE_KIND_REGISTER
					, .val.reg_num = type_is_floating(arg_rvalue.type) 
						? (R_FA0 + f_arg_count) 
						: (R_A0 + arg_count)
					, .type = arg_rvalue.type };

				emit_store_rvalue_to_rvalue(info, tmp, arg_rvalue);

				// Запоминаем, куда положили текущее значение, лежавшее в регистре-аргументе
				prev_arg_displ[arg_reg_count++] = tmp_arg_lvalue;
			}

			if (type_is_floating(arg_rvalue.type))
			{
				f_arg_count += 2;
			}
			else
			{
				arg_count += 1;
			}

			free_rvalue(info, arg_rvalue);
		}

		if (func_ref >= BEGIN_USER_FUNC)
		{
			// FIXME:
			uni_printf(info->sx->io, "\n\tjal FUNC%zu\n", func_ref);
		}
		else
		{
			// builtin
			/*
			uni_printf(info->sx->io, "\n");
			bi_func(info->sx->io, func_ref);
			uni_printf(info->sx->io, "\t");
			instruction_to_io(info->sx->io, IC_MIPS_NOP);
			uni_printf(info->sx->io, "\n");
			*/
		}

		// Восстановление регистров-аргументов -- они могут понадобится в дальнейшем
		uni_printf(info->sx->io, "\n\t# data restoring:\n");

		size_t i = 0, j = 0; // Счётчик обычных и floating point регистров-аргументов соответственно
		while (i + j < arg_reg_count)
		{
			uni_printf(info->sx->io, "\n");

			const rvalue tmp_rval = emit_load_of_lvalue(info, prev_arg_displ[i + j]);
			emit_store_rvalue_to_rvalue(info
				, (rvalue){ .from_lvalue = FROM_LVALUE
					, .kind = RVALUE_KIND_REGISTER
					, .val.reg_num = type_is_floating(prev_arg_displ[i + j].type)
						? (R_FA0 + 2 * j++)
						: (R_A0 + i++) }
				, tmp_rval);

			free_rvalue(info, tmp_rval);
		}

		if (displ_for_parameters)
		{
			to_code_2R_I(info->sx->io, IC_MIPS_ADDI, R_FP, R_FP, (item_t)displ_for_parameters);
		}

		uni_printf(info->sx->io, "\n");
	}

	return (rvalue){ .kind = RVALUE_KIND_REGISTER,
					 .type = return_type,
					 .val.reg_num = type_is_floating(return_type) ? R_FV0 : R_V0,
					 .from_lvalue = !FROM_LVALUE };
}

/**
 *	Emit increment/decrement expression
 *
 * @param	info			Encoder
 * @param	nd				Node in AST
 *
 * @return Rvalue of the result of increment/decrement expression
 */
static rvalue emit_inc_dec_expression(information *const info, const node *const nd)
{
	const unary_t operation = expression_unary_get_operator(nd);

	const node identifier = expression_unary_get_operand(nd);

	// TODO: вещественные числа
	lvalue identifier_lvalue = emit_lvalue(info, &identifier);
	const rvalue identifier_rvalue = emit_load_of_lvalue(info, identifier_lvalue);

	const mips_register_t post_result_reg = get_register(info);
	const rvalue post_result_rvalue = {
		.kind = RVALUE_KIND_REGISTER, .val.reg_num = post_result_reg, .type = identifier_lvalue.type, .from_lvalue = FROM_LVALUE
	};

	if (operation == UN_POSTDEC || operation == UN_POSTINC)
	{
		emit_store_rvalue_to_rvalue(info, post_result_rvalue, identifier_rvalue);
	}

	switch (operation)
	{
		case UN_PREDEC:
		case UN_POSTDEC:
			emit_binary_operation(info, identifier_rvalue, rvalue_negative_one, BIN_ADD);
			break;
		case UN_PREINC:
		case UN_POSTINC:
			emit_binary_operation(info, identifier_rvalue, rvalue_one, BIN_ADD);
			break;

		default:
			break;
	}

	if (identifier_lvalue.kind == LVALUE_KIND_STACK)
	{
		emit_store_of_rvalue(info, identifier_rvalue, identifier_lvalue);
	}
	// Иначе результат и так будет в identifier_rvalue

	if (operation == UN_POSTDEC || operation == UN_POSTINC)
	{
		free_rvalue(info, identifier_rvalue);
		return post_result_rvalue;
	}

	free_rvalue(info, post_result_rvalue);
	return identifier_rvalue;
}

/**
 *	Emit unary expression
 *
 * @param	info			Encoder
 * @param	nd				Node in AST
 *
 * @return	Rvalue of the result of unary expression
 */
static rvalue emit_unary_expression_rvalue(information *const info, const node *const nd)
{
	const unary_t operator= expression_unary_get_operator(nd);

	assert(operator!= UN_INDIRECTION);

	switch (operator)
	{
		case UN_POSTINC:
		case UN_POSTDEC:
		case UN_PREINC:
		case UN_PREDEC:
			return emit_inc_dec_expression(info, nd);

		case UN_MINUS:
		case UN_NOT:
		{
			const node operand = expression_unary_get_operand(nd);
			rvalue operand_rvalue = emit_expression(info, &operand);

			if (operator== UN_MINUS)
			{
				return emit_binary_operation(
					info,
					(rvalue){
						.kind = RVALUE_KIND_REGISTER, .val.reg_num = R_ZERO, .type = TYPE_INTEGER, .from_lvalue = FROM_LVALUE },
					operand_rvalue, BIN_SUB);
			}
			else
			{
				return emit_binary_operation(info, operand_rvalue, rvalue_negative_one, BIN_XOR);
			}
		}

		case UN_LOGNOT:
		{
			const node operand = expression_unary_get_operand(nd);
			rvalue operand_rvalue = emit_expression(info, &operand);

			const item_t curr_label_num = info->label_num++;
			const label label_else = { .kind = L_ELSE, .num = curr_label_num };
			const label label_end = { .kind = L_END, .num = curr_label_num };

			const rvalue result_rvalue = (rvalue){
				.kind = RVALUE_KIND_REGISTER, .val.reg_num = get_register(info), .type = TYPE_BOOLEAN, .from_lvalue = !FROM_LVALUE
			};

			uni_printf(info->sx->io, "\t");
			instruction_to_io(info->sx->io, IC_MIPS_BNE);
			uni_printf(info->sx->io, " ");
			rvalue_to_io(info, operand_rvalue);
			uni_printf(info->sx->io, ", $0, ");
			emit_label(info, label_else);

			uni_printf(info->sx->io, "\t");
			instruction_to_io(info->sx->io, IC_MIPS_LI);
			uni_printf(info->sx->io, " ");
			rvalue_to_io(info, result_rvalue);
			uni_printf(info->sx->io, ", 1\n");
			emit_unconditional_branch(info, label_end);

			emit_label_declaration(info, label_else);
			uni_printf(info->sx->io, "\t");
			instruction_to_io(info->sx->io, IC_MIPS_LI);
			uni_printf(info->sx->io, " ");
			rvalue_to_io(info, result_rvalue);
			uni_printf(info->sx->io, ", 0\n");

			emit_label_declaration(info, label_end);

			uni_printf(info->sx->io, "\n");

			return result_rvalue;
		}

		case UN_ABS:
		{
			const node operand = expression_unary_get_operand(nd);
			rvalue operand_rvalue = emit_expression(info, &operand);

			const item_t curr_label_num = info->label_num++;
			const label label_else = { .kind = L_ELSE, .num = curr_label_num };
			const label label_end = { .kind = L_END, .num = curr_label_num };

			uni_printf(info->sx->io, "\n\t");
			instruction_to_io(info->sx->io, IC_MIPS_BGEZ);
			uni_printf(info->sx->io, " ");
			rvalue_to_io(info, operand_rvalue);
			uni_printf(info->sx->io, ", ");
			emit_label(info, label_else);

			const rvalue result_rvalue = emit_binary_operation(info
				, (rvalue){ .kind = RVALUE_KIND_REGISTER
					, .val.reg_num = R_ZERO
					, .type = TYPE_INTEGER
					, .from_lvalue = FROM_LVALUE }
				, operand_rvalue, BIN_SUB);

			emit_label_declaration(info, label_end);

			return result_rvalue;
		}

		case UN_ADDRESS:
		{
			const node operand = expression_unary_get_operand(nd);
			const lvalue operand_lvalue = emit_lvalue(info, &operand);

			assert(operand_lvalue.kind != LVALUE_KIND_REGISTER);

			const rvalue result_rvalue = (rvalue){
				.from_lvalue = !FROM_LVALUE, .kind = RVALUE_KIND_REGISTER, .type = TYPE_INTEGER, .val.reg_num = get_register(info)
			};

			uni_printf(info->sx->io, "\n\t");
			instruction_to_io(info->sx->io, IC_MIPS_ADDI);
			uni_printf(info->sx->io, " ");
			rvalue_to_io(info, result_rvalue);
			uni_printf(info->sx->io, ", ");
			mips_register_to_io(info->sx->io, operand_lvalue.base_reg);
			uni_printf(info->sx->io, ", %" PRIitem "\n", operand_lvalue.loc.displ); // operand -- всегда переменная

			return result_rvalue;
		}

		default:
			// TODO: оставшиеся унарные операторы
			return (rvalue){ .kind = RVALUE_KIND_VOID };
	}
}

/**
 *	Emit logic binary expression
 *
 * @param	info			Encoder
 * @param	nd				Node in AST
 *
 * @return	Rvalue of the result of logic expression
 */
static rvalue emit_logic_expression(information *const info, const node *const nd)
{
	binary_t operation = expression_binary_get_operator(nd);

	const node LHS = expression_binary_get_LHS(nd);
	const rvalue lhs_rvalue = emit_expression(info, &LHS);

	operation = reverse_logic_command(operation);

	const node RHS = expression_binary_get_RHS(nd);
	const rvalue rhs_rvalue = emit_expression(info, &RHS);

	return emit_binary_operation(info, lhs_rvalue, rhs_rvalue, operation);
}

/**
 *	Emit non-assignment binary expression
 *
 *	@param	info			Encoder
 *	@param	nd				Node in AST
 */
static rvalue emit_integral_expression_rvalue(information *const info, const node *const nd)
{
	const binary_t operator= expression_binary_get_operator(nd);

	const node LHS = expression_binary_get_LHS(nd);
	rvalue lhs_rvalue = emit_expression(info, &LHS);

	const node RHS = expression_binary_get_RHS(nd);
	rvalue rhs_rvalue = emit_expression(info, &RHS);

	const rvalue res = emit_binary_operation(info, lhs_rvalue, rhs_rvalue, operator);

	return res;
}

/**
 * Emit assignment expression
 *
 * @param	info			Encoder
 * @param	nd				Node in AST
 *
 * @return	Rvalue of the result of assignment expression
 */
static rvalue emit_assignment_expression(information *const info, const node *const nd)
{
	const binary_t operation = expression_assignment_get_operator(nd);

	// LHS -- точно lvalue
	const node LHS = expression_assignment_get_LHS(nd);
	const lvalue lhs_lvalue = emit_lvalue(info, &LHS);

	const node RHS = expression_assignment_get_RHS(nd);
	rvalue rhs_rvalue = emit_expression(info, &RHS);

	if (operation != BIN_ASSIGN) // это "+=", "-=" и т.п.
	{
		const rvalue lhs_rvalue = emit_load_of_lvalue(info, lhs_lvalue);
		binary_t correct_operation;
		switch (operation)
		{
			case BIN_ADD_ASSIGN:
				correct_operation = BIN_ADD;
				break;

			case BIN_SUB_ASSIGN:
				correct_operation = BIN_SUB;
				break;

			case BIN_MUL_ASSIGN:
				correct_operation = BIN_MUL;
				break;

			case BIN_DIV_ASSIGN:
				correct_operation = BIN_DIV;
				break;

			case BIN_SHL_ASSIGN:
				correct_operation = BIN_SHL;
				break;

			case BIN_SHR_ASSIGN:
				correct_operation = BIN_SHR;
				break;

			case BIN_AND_ASSIGN:
				correct_operation = BIN_AND;
				break;

			case BIN_XOR_ASSIGN:
				correct_operation = BIN_XOR;
				break;

			default: // BIN_OR_ASSIGN
				correct_operation = BIN_OR;
		}
		rhs_rvalue = emit_binary_operation(info, lhs_rvalue, rhs_rvalue, correct_operation);

		free_rvalue(info, lhs_rvalue);
	}

	if (lhs_lvalue.kind == LVALUE_KIND_STACK)
	{
		emit_store_of_rvalue(info, rhs_rvalue, lhs_lvalue);
	}
	// Иначе всё и так будет в rhs_rvalue

	return rhs_rvalue;
}

/**
 * Emit binary expression
 *
 * @param	info			Encoder
 * @param	nd				Node in AST
 *
 * @return	Rvalue of the result of binary expression
 */
static rvalue emit_binary_expression(information *const info, const node *const nd)
{
	const binary_t operation = expression_binary_get_operator(nd);

	switch (operation)
	{
		case BIN_MUL:
		case BIN_DIV:
		case BIN_REM:
		case BIN_ADD:
		case BIN_SUB:
		case BIN_SHL:
		case BIN_SHR:
		case BIN_AND:
		case BIN_XOR:
		case BIN_OR:
			return emit_integral_expression_rvalue(info, nd);

		case BIN_LT:
		case BIN_GT:
		case BIN_LE:
		case BIN_GE:
		case BIN_EQ:
		case BIN_NE:
			return emit_logic_expression(info, nd);

		case BIN_LOG_OR:
		case BIN_LOG_AND:
		{
			const item_t curr_label_num = info->label_num++;
			const label label_end = { .kind = L_END, .num = curr_label_num };

			const node LHS = expression_binary_get_LHS(nd);
			rvalue result_rvalue = emit_expression(info, &LHS);

			if (result_rvalue.kind == RVALUE_KIND_CONST)
			{
				// Предварительно загружаем константу в rvalue вида RVALUE_KIND_REGISTER
				const rvalue tmp_rval = result_rvalue;
				result_rvalue = (rvalue){ .kind = RVALUE_KIND_REGISTER
					, .val.reg_num = type_is_floating(tmp_rval.type) 
						? get_float_register(info) 
						: get_register(info)
					, .type = tmp_rval.type
					, .from_lvalue = !FROM_LVALUE };
				emit_store_rvalue_to_rvalue(info, result_rvalue, tmp_rval);
			}

			uni_printf(info->sx->io, "\t");
			instruction_to_io(info->sx->io, (operation == BIN_LOG_OR) ? IC_MIPS_BNE : IC_MIPS_BEQ);
			uni_printf(info->sx->io, " ");
			rvalue_to_io(info, result_rvalue);
			uni_printf(info->sx->io, ", $0, ");
			emit_label(info, label_end);

			const node RHS = expression_binary_get_RHS(nd);
			const rvalue rhs_rvalue = emit_expression(info, &RHS);

			emit_store_rvalue_to_rvalue(info, result_rvalue, rhs_rvalue);

			free_rvalue(info, rhs_rvalue);

			emit_label_declaration(info, label_end);

			return result_rvalue;
		}

		default:
			// TODO: оставшиеся бинарные операторы
			return (rvalue){ .kind = RVALUE_KIND_VOID };
	}
}

/**
 * Emit ternary expression
 *
 * @param	info			Codegen info (?)
 * @param	nd				Node in AST
 *
 * @return	Rvalue of the result of ternary expression
 */
static rvalue emit_ternary_expression(information *const info, const node *const nd)
{
	uni_printf(info->sx->io, "\n\t# ternary expression:\n");

	const size_t curr_label_num = info->label_num++;
	const label old_label_else = info->label_else;
	const label label_else = { .kind = L_ELSE, .num = curr_label_num };
	const label label_end = { .kind = L_END, .num = curr_label_num };

	const node condition = expression_ternary_get_condition(nd);
	const rvalue condition_rvalue = emit_expression(info, &condition);

	// if <значение из condition_rvalue> == 0 -- прыжок в else
	uni_printf(info->sx->io, "\n\t");
	instruction_to_io(info->sx->io, IC_MIPS_BEQ);
	uni_printf(info->sx->io, " ");
	rvalue_to_io(info, condition_rvalue);
	uni_printf(info->sx->io, ", $0, ");
	emit_label(info, label_else);

	free_rvalue(info, condition_rvalue);

	const rvalue result_rvalue = (rvalue) { .kind = RVALUE_KIND_REGISTER
		, .val.reg_num = type_is_floating(expression_get_type(nd)) ? get_float_register(info) : get_register(info)
		, .type = expression_get_type(nd)
		, .from_lvalue = !FROM_LVALUE };

	const node then = expression_ternary_get_LHS(nd);
	const rvalue lhs_rvalue = emit_expression(info, &then);

	emit_store_rvalue_to_rvalue(info, result_rvalue, lhs_rvalue);

	free_rvalue(info, lhs_rvalue);

	info->label_else = old_label_else;

	emit_unconditional_branch(info, label_end);
	emit_label_declaration(info, label_else);

	const node else_substmt = expression_ternary_get_RHS(nd);
	const rvalue rhs_rvalue = emit_expression(info, &else_substmt);

	emit_store_rvalue_to_rvalue(info, result_rvalue, rhs_rvalue);

	free_rvalue(info, rhs_rvalue);

	emit_label_declaration(info, label_end);

	uni_printf(info->sx->io, "\n");

	return result_rvalue;
}

/**
 * Emit inline expression
 *
 * @param	info			Codegen info (?)
 * @param	nd				Node in AST
 *
 * @return	Rvalue of inline expression
 */
static rvalue emit_inline_expression(information *const info, const node *const nd)
{
	// FIXME: inline expression cannot return value at the moment
	const size_t amount = expression_inline_get_size(nd);

	for (size_t i = 0; i < amount; i++)
	{
		const node substmt = expression_inline_get_substmt(nd, i);
		emit_statement(info, &substmt);
	}

	return (rvalue){ .kind = RVALUE_KIND_VOID };
}

/**
 * Emit expression
 *
 * @param	info			Codegen info (?)
 * @param	nd				Node in AST
 *
 * @return	Rvalue of the expression
 */
static rvalue emit_expression(information *const info, const node *const nd)
{
	if (expression_is_lvalue(nd))
	{
		lvalue lval = emit_lvalue(info, nd);
		return emit_load_of_lvalue(info, lval);
	}

	// Иначе rvalue:
	switch (expression_get_class(nd))
	{
		case EXPR_CAST:
			return emit_cast_expression(info, nd);

		case EXPR_LITERAL:
			return emit_literal_expression(info->sx, nd);

		case EXPR_CALL:
			return emit_call_expression(info, nd);

		case EXPR_UNARY:
			return emit_unary_expression_rvalue(info, nd);

		case EXPR_BINARY:
			return emit_binary_expression(info, nd);

		case EXPR_ASSIGNMENT:
			return emit_assignment_expression(info, nd);

		case EXPR_TERNARY:
			return emit_ternary_expression(info, nd);

		case EXPR_INLINE:
			return emit_inline_expression(info, nd);

		case EXPR_INITIALIZER:
			// FIXME: кидать соответствующую ошибку
			assert(expression_get_class(nd) != EXPR_INITIALIZER);
			return (rvalue){ .kind = RVALUE_KIND_VOID };

		default: // EXPR_INVALID
			// TODO: генерация оставшихся выражений
			assert(expression_get_class(nd) != EXPR_INVALID);
			return (rvalue){ .kind = RVALUE_KIND_VOID };
	}
}

/**
 * Emit expression which will be evaluated as a void expression
 *
 * @param	info			Encoder
 * @param	nd				Node in AST
 *
 * @return	Rvalue of void type
 */
static rvalue emit_void_expression(information *const info, const node *const nd)
{
	if (expression_is_lvalue(nd))
	{
		emit_lvalue(info, nd); // Либо регистровая переменная, либо на стеке => ничего освобождать не надо
	}
	else
	{
		const rvalue result = emit_expression(info, nd);

		free_rvalue(info, result);
	}
	return (rvalue){ .kind = RVALUE_KIND_VOID };
}

static rvalue emit_boolean_expression(information *const info, const node *const nd)
{
	const rvalue value = emit_expression(info, nd);
	assert(type_is_scalar(info->sx, value.type));

	const bool is_integer = !type_is_floating(value.type);

	if (is_integer)
	{
		return value;
	}

	if (value.kind == RVALUE_KIND_CONST)
	{
		return (rvalue){ .kind = RVALUE_KIND_CONST, .type = TYPE_INTEGER, .val.int_val = value.val.float_val ? 1 : 0 };
	}

	// Only register kind and floating point type remains
	// TODO: проверить!
	const rvalue result = {
		.from_lvalue = !FROM_LVALUE, .kind = RVALUE_KIND_REGISTER, .val.reg_num = get_register(info), .type = TYPE_INTEGER
	};
	to_code_2R(info->sx->io, IC_MIPS_CVT_W_S, value.val.reg_num, value.val.reg_num);
	to_code_2R(info->sx->io, IC_MIPS_MFC_1, value.val.reg_num, result.val.reg_num);

	free_rvalue(info, value);

	return result;
}


/*
 *	 _____     ______     ______     __         ______     ______     ______     ______   __     ______     __   __     ______
 *	/\  __-.  /\  ___\   /\  ___\   /\ \       /\  __ \   /\  == \   /\  __ \   /\__  _\ /\ \   /\  __ \   /\ "-.\ \   /\  ___\
 *	\ \ \/\ \ \ \  __\   \ \ \____  \ \ \____  \ \  __ \  \ \  __<   \ \  __ \  \/_/\ \/ \ \ \  \ \ \/\ \  \ \ \-.  \  \ \___  \
 *	 \ \____-  \ \_____\  \ \_____\  \ \_____\  \ \_\ \_\  \ \_\ \_\  \ \_\ \_\    \ \_\  \ \_\  \ \_____\  \ \_\\"\_\  \/\_____\
 *	  \/____/   \/_____/   \/_____/   \/_____/   \/_/\/_/   \/_/ /_/   \/_/\/_/     \/_/   \/_/   \/_____/   \/_/ \/_/   \/_____/
 */


/**
 * Emit array declaration
 *
 * @param	info			Codegen info (?)
 * @param	nd				Node in AST
 */
static void emit_array_declaration(information *const info, const node *const nd)
{
	const size_t identifier = declaration_variable_get_id(nd);
	item_t type = ident_get_type(info->sx, identifier);
	item_t dimensions = 0;
	const size_t arr_displ = hash_get(&info->displacements, identifier, 1);
	bool has_empty_bounds = false;

	// Сохраняем адрес начала массива
	emit_store_of_rvalue(info
		, (rvalue){ .from_lvalue = !FROM_LVALUE, .kind = RVALUE_KIND_REGISTER, .val.reg_num = R_FP, .type = TYPE_INTEGER }
		, (lvalue){ .base_reg = R_SP, .kind = LVALUE_KIND_STACK, .loc.displ = arr_displ, .type = TYPE_ARRAY });

	while (type_is_array(info->sx, type))
	{
		type = type_array_get_element_type(info->sx, type);
		const node bound = declaration_variable_get_bound(nd, (size_t)dimensions);
		if (expression_get_class(&bound) == EXPR_EMPTY_BOUND)
		{
			if (type_is_array(info->sx, type))
			{
				system_error(empty_init, &bound);
			}

			has_empty_bounds = true;
		}
		else
		{
			rvalue bound_rvalue = emit_expression(info, &bound);
			if (bound_rvalue.kind == RVALUE_KIND_CONST)
			{
				// Предварительно загружаем константу в rvalue вида RVALUE_KIND_REGISTER
				const rvalue tmp_rval = bound_rvalue;
				bound_rvalue = (rvalue){ .kind = RVALUE_KIND_REGISTER
					, .val.reg_num = type_is_floating(tmp_rval.type) 
						? get_float_register(info)
						: get_register(info)
					, .type = tmp_rval.type
					, .from_lvalue = !FROM_LVALUE };
				emit_store_rvalue_to_rvalue(info, bound_rvalue, tmp_rval);
			}

			// Размещаем размер текущего измерения
			emit_store_of_rvalue(info
				, bound_rvalue
				, (lvalue){ .base_reg = R_FP, .kind = LVALUE_KIND_STACK, .loc.displ = 0, .type = TYPE_INTEGER });

			// Смещаем на один (за размер)
			emit_binary_operation(info, bound_rvalue, rvalue_one, BIN_ADD);

			// Умножаем на WORD_LENGTH
			emit_binary_operation(info
				, bound_rvalue
				, (rvalue){ .from_lvalue = !FROM_LVALUE
					, .kind = RVALUE_KIND_CONST
					, .type = TYPE_INTEGER
					, .val.int_val = WORD_LENGTH }
				, BIN_MUL);

			bound_rvalue.from_lvalue = FROM_LVALUE; // Чтобы в bound_rvalue не записался результат следующей функции
			// Сдвигаем $fp
			emit_binary_operation(info
				, (rvalue){ .from_lvalue = !FROM_LVALUE
					, .kind = RVALUE_KIND_REGISTER
					, .type = TYPE_INTEGER
					, .val.reg_num = R_FP }
				, bound_rvalue, BIN_SUB);

			free_rvalue(info, bound_rvalue);
		}

		dimensions++;
	}

	// Смещаемся, чтобы $fp ни на что не указывал
	uni_printf(info->sx->io, "\n\t# setting up $fp:\n");
	to_code_2R_I(info->sx->io, IC_MIPS_ADDI, R_FP, R_FP, -(item_t)WORD_LENGTH);
	uni_printf(info->sx->io, "\n");

	if (declaration_variable_has_initializer(nd))
	{
		// смещение от $sp, где хранится адрес массива
		const item_t displ = hash_get(&info->displacements, identifier, 1);
		const rvalue array_address_rvalue = emit_load_of_lvalue(info
			, (lvalue){ .base_reg = R_SP, .kind = LVALUE_KIND_STACK, .loc.displ = displ, .type = type });
		// Теперь имеем адрес начала массива

		node init = declaration_variable_get_initializer(nd);
		const size_t amount = expression_initializer_get_size(&init);
		// TODO: многомерные массивы
		for (size_t i = 0; i < amount; i++)
		{
			const node subexpr = expression_initializer_get_subexpr(&init, i);
			const rvalue subexpr_rval = emit_expression(info, &subexpr);

			emit_store_of_rvalue(info
				, subexpr_rval
				, (lvalue){ .kind = LVALUE_KIND_STACK
					, .base_reg = array_address_rvalue.val.reg_num // Гарантируется, что rvalue типа регистр
					, .type = expression_get_type(&subexpr)
					, .loc.displ = -(item_t)(i + 1) * WORD_LENGTH });

			free_rvalue(info, subexpr_rval);
		}

		free_rvalue(info, array_address_rvalue);
	}
}

/**
 * Emit variable declaration
 *
 * @param	info			Codegen info (?)
 * @param	nd				Node in AST
 */
static void emit_variable_declaration(information *const info, const node *const nd)
{
	const size_t identifier = declaration_variable_get_id(nd);

	const lvalue variable = displacements_add(info, identifier);

	const item_t type = ident_get_type(info->sx, identifier);
	if (type_is_array(info->sx, type))
	{
		if (declaration_variable_has_initializer(nd))
		{
			const node initializer = declaration_variable_get_initializer(nd);
			const rvalue value = emit_expression(info, &initializer);

			emit_store_of_rvalue(info, value, variable);
			free_rvalue(info, value);
		}
	}
	else
	{
		emit_array_declaration(info, nd);
	}
}

/**
 * Emit function definition
 *
 * @param	info			Encoder
 * @param	nd				Node in AST
 */
static void emit_function_definition(information *const info, const node *const nd)
{
	const size_t ref_ident = declaration_function_get_id(nd);
	const label func_label = { .kind = L_FUNC, .num = ref_ident };
	emit_label_declaration(info, func_label);
	uni_printf(info->sx->io, "\t# \"%s\" function:\n", ident_get_spelling(info->sx, ref_ident));

	const item_t func_type = ident_get_type(info->sx, ref_ident);
	const size_t parameters = type_function_get_parameter_amount(info->sx, func_type);

	info->curr_function_ident = ref_ident;
	info->max_displ = 0;

	// Сохранение оберегаемых регистров перед началом работы функции
	// FIXME: избавиться от функций to_code:
	uni_printf(info->sx->io, "\n\t# preserved registers:\n");
	to_code_R_I_R(info->sx->io, IC_MIPS_SW, R_RA, -(item_t)RA_SIZE, R_FP);
	to_code_R_I_R(info->sx->io, IC_MIPS_SW, R_SP, -(item_t)(RA_SIZE + SP_SIZE), R_FP);

	// Сохранение s0-s7
	for (size_t i = 0; i < PRESERVED_REG_AMOUNT; i++)
	{
		to_code_R_I_R(info->sx->io, IC_MIPS_SW, R_S0 + i, -(item_t)(RA_SIZE + SP_SIZE + (i + 1) * WORD_LENGTH), R_FP);
	}

	uni_printf(info->sx->io, "\n");

	// Сохранение fs0-fs10 (в цикле 5, т.к. операции одинарной точности => нужны только четные регистры)
	for (size_t i = 0; i < PRESERVED_FP_REG_AMOUNT/2; i++)
	{
		to_code_R_I_R(info->sx->io, IC_MIPS_S_S, R_FS0 + 2 * i
			, -(item_t)(RA_SIZE + SP_SIZE + (i + 1) * WORD_LENGTH + PRESERVED_REG_AMOUNT*WORD_LENGTH /* за $s0-$s7 */)
			, R_FP);
	}

	// Сохранение $a0-$a3:
	for (size_t i = 0; i < ARG_REG_AMOUNT; i++)
	{
		to_code_R_I_R(info->sx->io
			, IC_MIPS_SW, R_A0 + i
			, -(item_t)(RA_SIZE + SP_SIZE + (i + 1) * WORD_LENGTH + PRESERVED_REG_AMOUNT * WORD_LENGTH /* за $s0-$s7 */
				+ PRESERVED_FP_REG_AMOUNT/2 * WORD_LENGTH /* за $fs0-$fs10 */)
			, R_FP);
	}

	// Выравнивание смещения на 8
	if (info->max_displ % 8)
	{
		const size_t padding = 8 - (info->max_displ % 8);
		info->max_displ += padding;
		if (padding)
		{
			uni_printf(info->sx->io, "\n\t# padding -- max displacement == %zu\n", info->max_displ);
		}
	}

	// Создание буфера для тела функции
	universal_io *old_io = info->sx->io;
	universal_io new_io = io_create();
	out_set_buffer(&new_io, BUFFER_SIZE);
	info->sx->io = &new_io;

	uni_printf(info->sx->io, "\n\t# function parameters:\n");

	size_t gpr_count = 0;
	size_t fp_count = 0;

	for (size_t i = 0; i < parameters; i++)
	{
		const size_t id = declaration_function_get_parameter(nd, i);

		// Вносим переменную в таблицу символов
		const size_t index = hash_add(&info->displacements, id, 2);

		uni_printf(info->sx->io, "\t# parameter \"%s\" ", ident_get_spelling(info->sx, id));

		// TODO: TYPE_FLOATING
		if (!type_is_floating(ident_get_type(info->sx, id)))
		{
			if (i < ARG_REG_AMOUNT)
			{
				// Рассматриваем их как регистровые переменные
				const mips_register_t curr_reg = R_A0 + gpr_count++;
				uni_printf(info->sx->io, "is in register ");
				mips_register_to_io(info->sx->io, curr_reg);
				uni_printf(info->sx->io, "\n");

				hash_set_by_index(&info->displacements, index, 0, !IS_ON_STACK);
				hash_set_by_index(&info->displacements, index, 1, (item_t)curr_reg);
			}
			else
			{
				// TODO:
				assert(false);
			}
		}
		else
		{
			if (i < ARG_REG_AMOUNT/2)
			{
				// Рассматриваем их как регистровые переменные
				const mips_register_t curr_reg = R_FA0 + 2*fp_count++;
				uni_printf(info->sx->io, "is in register ");
				mips_register_to_io(info->sx->io, curr_reg);
				uni_printf(info->sx->io, "\n");

				hash_set_by_index(&info->displacements, index, 0, !IS_ON_STACK);
				hash_set_by_index(&info->displacements, index, 1, (item_t)curr_reg);
			}
			else
			{
				// TODO:
				assert(false);
			}
		}
	}

	uni_printf(info->sx->io, "\n\t# function body:\n");
	node body = declaration_function_get_body(nd);
	emit_statement(info, &body);

	// Извлечение буфера с телом функции в старый io
	char *buffer = out_extract_buffer(info->sx->io);
	info->sx->io = old_io;

	uni_printf(info->sx->io, "\n\t# setting up $fp:\n");
	// $fp указывает на конец динамики (которое в данный момент равно концу статики)
	to_code_2R_I(info->sx->io, IC_MIPS_ADDI, R_FP, R_FP, -(item_t)(info->max_displ + FUNC_DISPL_PRESEREVED + WORD_LENGTH));

	uni_printf(info->sx->io, "\n\t# setting up $sp:\n");
	// $sp указывает на конец статики (которое в данный момент равно концу динамики)
	to_code_2R(info->sx->io, IC_MIPS_MOVE, R_SP, R_FP);

	// Смещаем $fp ниже конца статики (чтобы он не совпадал с $sp)
	to_code_2R_I(info->sx->io, IC_MIPS_ADDI, R_FP, R_FP, -(item_t)WORD_LENGTH);

	uni_printf(info->sx->io, "%s", buffer);
	free(buffer);

	const label end_label = { .kind = L_END, .num = ref_ident };
	emit_label_declaration(info, end_label);

	// Восстановление стека после работы функции
	uni_printf(info->sx->io, "\n\t# data restoring:\n");

	// Ставим $fp на его положение в предыдущей функции
	to_code_2R_I(info->sx->io, IC_MIPS_ADDI, R_FP, R_SP,
				 (item_t)(info->max_displ + FUNC_DISPL_PRESEREVED + WORD_LENGTH));

	uni_printf(info->sx->io, "\n");

	// Восстановление $s0-$s7
	for (size_t i = 0; i < PRESERVED_REG_AMOUNT; i++)
	{
		to_code_R_I_R(info->sx->io, IC_MIPS_LW, R_S0 + i, -(item_t)(RA_SIZE + SP_SIZE + (i + 1) * WORD_LENGTH), R_FP);
	}

	uni_printf(info->sx->io, "\n");

	// Восстановление $fs0-$fs7
	for (size_t i = 0; i < PRESERVED_FP_REG_AMOUNT/2; i++)
	{
		to_code_R_I_R(info->sx->io, IC_MIPS_L_S, R_FS0 + 2 * i
			, -(item_t)(RA_SIZE + SP_SIZE + (i + 1) * WORD_LENGTH + /* за s0-s7 */ 8 * WORD_LENGTH), R_FP);
	}

	// Восстановление $a0-$a3
	for (size_t i = 0; i < ARG_REG_AMOUNT; i++)
	{
		to_code_R_I_R(info->sx->io, IC_MIPS_LW, R_A0 + i
			, -(item_t)(RA_SIZE + SP_SIZE + (i + 1) * WORD_LENGTH + 8 * WORD_LENGTH /* за s0-s7 */
				+ 5 * WORD_LENGTH /* за $fs0-$fs10*/)
			, R_FP);
	}

	uni_printf(info->sx->io, "\n");

	// Возвращаем $sp его положение в предыдущей функции
	to_code_R_I_R(info->sx->io, IC_MIPS_LW, R_SP, -(item_t)(RA_SIZE + SP_SIZE), R_FP);

	to_code_R_I_R(info->sx->io, IC_MIPS_LW, R_RA, -(item_t)(RA_SIZE), R_FP);

	// Прыгаем далее
	to_code_R(info->sx->io, IC_MIPS_JR, R_RA);
}

static void emit_declaration(information *const info, const node *const nd)
{
	switch (declaration_get_class(nd))
	{
		case DECL_VAR:
			emit_variable_declaration(info, nd);
			break;

		case DECL_FUNC:
			emit_function_definition(info, nd);
			break;

		default:
			// С объявлением типа ничего делать не нужно
			return;
	}

	uni_printf(info->sx->io, "\n");
}


/*
 *	 ______     ______   ______     ______   ______     __    __     ______     __   __     ______   ______
 *	/\  ___\   /\__  _\ /\  __ \   /\__  _\ /\  ___\   /\ "-./  \   /\  ___\   /\ "-.\ \   /\__  _\ /\  ___\
 *	\ \___  \  \/_/\ \/ \ \  __ \  \/_/\ \/ \ \  __\   \ \ \-./\ \  \ \  __\   \ \ \-.  \  \/_/\ \/ \ \___  \
 *	 \/\_____\    \ \_\  \ \_\ \_\    \ \_\  \ \_____\  \ \_\ \ \_\  \ \_____\  \ \_\\"\_\    \ \_\  \/\_____\
 *	  \/_____/     \/_/   \/_/\/_/     \/_/   \/_____/   \/_/  \/_/   \/_____/   \/_/ \/_/     \/_/   \/_____/
 */


/**
 *	Emit declaration statement
 *
 *	@param	info		Information
 *	@param	nd			Node in AST
 */
static void emit_declaration_statement(information *const info, const node *const nd)
{
	const size_t size = statement_declaration_get_size(nd);
	for (size_t i = 0; i < size; i++)
	{
		const node decl = statement_declaration_get_declarator(nd, i);
		emit_declaration(info, &decl);
	}
}

/**
 * Emit compound statement
 *
 * @param	info			Encoder
 * @param	nd				Node in AST
 */
static void emit_compound_statement(information *const info, const node *const nd)
{
	const item_t scope_displacement = info->displ;

	const size_t size = statement_compound_get_size(nd);
	for (size_t i = 0; i < size; i++)
	{
		const node substmt = statement_compound_get_substmt(nd, i);
		emit_statement(info, &substmt);
	}

	info->displ = scope_displacement;
}

/**
 *	Emit if statement
 *
 *	@param	info			Encoder
 *	@param	nd				Node in AST
 */
static void emit_if_statement(information *const info, const node *const nd)
{
	const node condition = statement_if_get_condition(nd);
	const rvalue value = emit_boolean_expression(info, &condition);

	const size_t label_num = info->label_num++;
	const label label_else = { .kind = L_ELSE, .num = label_num };
	const label label_end = { .kind = L_END, .num = label_num };

	const bool has_else = statement_if_has_else_substmt(nd);
	emit_conditional_branch(info, value, has_else ? label_else : label_end);

	const node then_substmt = statement_if_get_then_substmt(nd);
	emit_statement(info, &then_substmt);

	if (has_else)
	{
		emit_unconditional_branch(info, label_end);
		emit_label_declaration(info, label_else);

		const node else_substmt = statement_if_get_else_substmt(nd);
		emit_statement(info, &else_substmt);
	}
	
	emit_label_declaration(info, label_end);
}

/**
 * Emit while statement
 *
 * @param	info			Encoder
 * @param	nd				Node in AST
 */
static void emit_while_statement(information *const info, const node *const nd)
{
	const size_t label_num = info->label_num++;
	const label label_begin = { .kind = L_BEGIN_CYCLE, .num = label_num };
	const label label_end = { .kind = L_END, .num = label_num };

	const label old_continue = info->label_continue;
	const label old_break = info->label_break;

	info->label_continue = label_begin;
	info->label_break = label_end;

	emit_label_declaration(info, label_begin);

	const node condition = statement_while_get_condition(nd);
	const rvalue value = emit_boolean_expression(info, &condition);

	emit_conditional_branch(info, value, label_end);

	const node body = statement_while_get_body(nd);
	emit_statement(info, &body);

	emit_unconditional_branch(info, label_begin);
	emit_label_declaration(info, label_end);

	info->label_continue = old_continue;
	info->label_break = old_break;
}

/**
 * Emit do statement
 *
 * @param	info			Encoder
 * @param	nd				Node in AST
 */
static void emit_do_statement(information *const info, const node *const nd)
{
	const size_t label_num = info->label_num++;
	const label label_begin = { .kind = L_BEGIN_CYCLE, .num = label_num };
	emit_label_declaration(info, label_begin);

	const label label_condition = { .kind = L_NEXT, .num = label_num };
	const label label_end = { .kind = L_END, .num = label_num };
	
	const label old_continue = info->label_continue;
	const label old_break = info->label_break;
	info->label_continue = label_condition;
	info->label_break = label_end;

	const node body = statement_do_get_body(nd);
	emit_statement(info, &body);
	emit_label_declaration(info, label_condition);

	const node condition = statement_do_get_condition(nd);
	const rvalue value = emit_boolean_expression(info, &condition);

	emit_conditional_branch(info, value, label_begin); // FIXME: в другую сторону
	emit_label_declaration(info, label_end);

	info->label_continue = old_continue;
	info->label_break = old_break;
}

/**
 * Emit for statement
 *
 * @param	info			Encoder
 * @param	nd				Node in AST
 */
static void emit_for_statement(information *const info, const node *const nd)
{
	const item_t scope_displacement = info->displ;
	if (statement_for_has_inition(nd))
	{
		const node inition = statement_for_get_inition(nd);
		emit_statement(info, &inition);
	}

	const size_t label_num = info->label_num++;
	const label label_begin = { .kind = L_BEGIN_CYCLE, .num = label_num };
	const label label_end = { .kind = L_END, .num = label_num };

	const label old_continue = info->label_continue;
	const label old_break = info->label_break;
	info->label_continue = label_begin;
	info->label_break = label_end;

	emit_label_declaration(info, label_begin);
	if (statement_for_has_condition(nd))
	{
		const node condition = statement_for_get_condition(nd);
		const rvalue value = emit_boolean_expression(info, &condition);
		emit_conditional_branch(info, value, label_end);
	}

	const node body = statement_for_get_body(nd);
	emit_statement(info, &body);

	if (statement_for_has_increment(nd))
	{
		const node increment = statement_for_get_increment(nd);
		emit_void_expression(info, &increment);
	}

	emit_unconditional_branch(info, label_begin);
	emit_label_declaration(info, label_end);

	info->label_continue = old_continue;
	info->label_break = old_break;
	info->displ = scope_displacement;
}

/**
 *	Emit continue statement
 *
 *	@param	info		Information
 */
static void emit_continue_statement(information *const info)
{
	emit_unconditional_branch(info, info->label_continue);
}

/**
 *	Emit break statement
 *
 *	@param	info		Information
 */
static void emit_break_statement(information *const info)
{
	emit_unconditional_branch(info, info->label_break);
}

/**
 * Emit return statement
 *
 * @param	info			Codegen info (?)
 * @param	nd				Node in AST
 */
static void emit_return_statement(information *const info, const node *const nd)
{
	if (statement_return_has_expression(nd))
	{
		const node expression = statement_return_get_expression(nd);
		const rvalue value = emit_expression(info, &expression);

		const item_t type = expression_get_type(nd);
		const lvalue return_lval = { .kind = LVALUE_KIND_REGISTER, .loc.reg_num = R_V0, .type = type };

		emit_store_of_rvalue(info, value, return_lval);

		free_rvalue(info, value);
	}

	const label label_end = { .kind = L_FUNCEND, .num = info->curr_function_ident };
	emit_unconditional_branch(info, label_end);
}

/**
 * Emit statement
 *
 * @param	info			Codegen info (?)
 * @param	nd				Node in AST
 */
static void emit_statement(information *const info, const node *const nd)
{
	switch (statement_get_class(nd))
	{
		case STMT_DECL:
			emit_declaration_statement(info, nd);
			break;

		case STMT_CASE:
			// emit_case_statement(info, nd);
			break;

		case STMT_DEFAULT:
			// emit_default_statement(info, nd);
			break;

		case STMT_COMPOUND:
			emit_compound_statement(info, nd);
			break;

		case STMT_EXPR:
			emit_void_expression(info, nd);
			break;

		case STMT_NULL:
			break;

		case STMT_IF:
			emit_if_statement(info, nd);
			break;

		case STMT_SWITCH:
			// emit_switch_statement(info, nd);
			break;

		case STMT_WHILE:
			emit_while_statement(info, nd);
			break;

		case STMT_DO:
			emit_do_statement(info, nd);
			break;

		case STMT_FOR:
			emit_for_statement(info, nd);
			break;

		case STMT_CONTINUE:
			emit_continue_statement(info);
			break;

		case STMT_BREAK:
			emit_break_statement(info);
			break;

		case STMT_RETURN:
			emit_return_statement(info, nd);
			break;

		default:
			break;
	}

	uni_printf(info->sx->io, "\n");
}

/**
 * Emit translation unit
 *
 * @param	info			Encoder
 * @param	nd				Node in AST
 */
static int emit_translation_unit(information *const info, const node *const nd)
{
	const size_t size = translation_unit_get_size(nd);
	for (size_t i = 0; i < size; i++)
	{
		const node decl = translation_unit_get_declaration(nd, i);
		emit_declaration(info, &decl);
	}

	return info->sx->rprt.errors != 0;
}


// В дальнейшем при необходимости сюда можно передавать флаги вывода директив
// TODO: подписать, что значит каждая директива и команда
static void pregen(syntax *const sx)
{
	// Подпись "GNU As:" для директив GNU
	// Подпись "MIPS Assembler:" для директив ассемблера MIPS

	uni_printf(sx->io, "\t.section .mdebug.abi32\n"); // ?
	uni_printf(sx->io, "\t.previous\n"); // следующая инструкция будет перенесена в секцию, описанную выше
	uni_printf(sx->io, "\t.nan\tlegacy\n");		  // ?
	uni_printf(sx->io, "\t.module fp=xx\n");	  // ?
	uni_printf(sx->io, "\t.module nooddspreg\n"); // ?
	uni_printf(sx->io, "\t.abicalls\n");		  // ?
	uni_printf(sx->io, "\t.option pic0\n"); // как если бы при компиляции была включена опция "-fpic" (что означает?)
	uni_printf(sx->io, "\t.text\n"); // последующий код будет перенесён в текстовый сегмент памяти
	// выравнивание последующих данных/команд по границе, кратной 2^n байт (в данном случае 2^2 = 4)
	uni_printf(sx->io, "\t.align 2\n");

	// делает метку main глобальной -- её можно вызывать извне кода (например, используется при линковке)
	uni_printf(sx->io, "\n\t.globl\tmain\n");
	uni_printf(sx->io, "\t.ent\tmain\n");			  // начало процедуры main
	uni_printf(sx->io, "\t.type\tmain, @function\n"); // тип "main" -- функция
	uni_printf(sx->io, "main:\n");

	// инициализация gp
	// "__gnu_local_gp" -- локация в памяти, где лежит Global Pointer
	uni_printf(sx->io, "\tlui $gp, %%hi(__gnu_local_gp)\n");
	uni_printf(sx->io, "\taddiu $gp, $gp, %%lo(__gnu_local_gp)\n");

	to_code_2R(sx->io, IC_MIPS_MOVE, R_FP, R_SP);
	to_code_2R_I(sx->io, IC_MIPS_ADDI, R_FP, R_FP, -4);
	to_code_R_I_R(sx->io, IC_MIPS_SW, R_RA, 0, R_FP);
	to_code_R_I(sx->io, IC_MIPS_LI, R_T0, LOW_DYN_BORDER);
	to_code_R_I_R(sx->io, IC_MIPS_SW, R_T0, -(item_t)HEAP_DISPL - 60, R_GP);
	uni_printf(sx->io, "\n");
}

// создаём метки всех строк в программе
static void strings_declaration(information *const info)
{
	uni_printf(info->sx->io, "\t.rdata\n");
	uni_printf(info->sx->io, "\t.align 2\n");

	const size_t amount = strings_amount(info->sx);
	for (size_t i = 0; i < amount; i++)
	{
		item_t args_for_printf = 0;
		const label string_label = { .kind = L_STRING, .num = i };
		emit_label(info, string_label);
		uni_printf(info->sx->io, "\t.ascii \"");

		const char *string = string_get(info->sx, i);
		for (size_t j = 0; string[j] != '\0'; j++)
		{
			const char ch = string[j];
			if (ch == '\n')
			{
				uni_printf(info->sx->io, "\\n");
			}
			else if (ch == '%')
			{
				args_for_printf++;
				j++;

				uni_printf(info->sx->io, "%c", ch);
				uni_printf(info->sx->io, "%c", string[j]);

				uni_printf(info->sx->io, "\\0\"\n");
				const label another_str_label = { .kind = L_STRING, .num = i + args_for_printf*amount };
				emit_label(info, another_str_label);
				uni_printf(info->sx->io, "\t.ascii \"");
			}
			else
			{
				uni_printf(info->sx->io, "%c", ch);
			}
		}

		uni_printf(info->sx->io, "\\0\"\n");
	}
	uni_printf(info->sx->io, "\t.text\n");
	uni_printf(info->sx->io, "\t.align 2\n\n");
}

// TODO: подписать, что значит каждая директива и команда
static void postgen(information *const info)
{
	// FIXME:
	uni_printf(info->sx->io, "\n\tjal FUNC%zu\n", info->sx->ref_main);
	to_code_R_I_R(info->sx->io, IC_MIPS_LW, R_RA, 0, R_FP);
	to_code_R(info->sx->io, IC_MIPS_JR, R_RA);

	// вставляем runtime.s в конец файла
	/*
	uni_printf(info->sx->io, "\n\n# runtime\n");
	char *runtime = "../runtimeMIPS/runtime.s";
	FILE *file = fopen(runtime, "r+");
	if (runtime != NULL)
	{
		char string[1024];
		while (fgets(string, sizeof(string), file) != NULL)
		{
			uni_printf(info->sx->io, "%s", string);
		}
	}
	fclose(file);
	uni_printf(info->sx->io, "# runtime end\n\n");

	uni_printf(info->sx->io, "\t.end\tmain\n");
	uni_printf(info->sx->io, "\t.size\tmain, .-main\n");
	// TODO: тут ещё часть вывод таблицы типов должен быть (вроде это для написанных самими функции типа printid)
	*/
}


/*
 *	 __	 __   __	 ______   ______	 ______	 ______   ______	 ______	 ______
 *	/\ \   /\ "-.\ \   /\__  _\ /\  ___\   /\  == \   /\  ___\ /\  __ \   /\  ___\   /\  ___\
 *	\ \ \  \ \ \-.  \  \/_/\ \/ \ \  __\   \ \  __<   \ \  __\ \ \  __ \  \ \ \____  \ \  __\
 *	 \ \_\  \ \_\\"\_\	\ \_\  \ \_____\  \ \_\ \_\  \ \_\	\ \_\ \_\  \ \_____\  \ \_____\
 *	  \/_/   \/_/ \/_/	 \/_/   \/_____/   \/_/ /_/   \/_/	 \/_/\/_/   \/_____/   \/_____/
 */


int encode_to_mips(const workspace *const ws, syntax *const sx)
{
	if (!ws_is_correct(ws) || sx == NULL)
	{
		return -1;
	}

	information info;
	info.sx = sx;
	info.next_register = R_T0;
	info.next_float_register = R_FT0;
	info.label_num = 1;

	info.displacements = hash_create(HASH_TABLE_SIZE);

	pregen(sx);
	strings_declaration(&info);
	// TODO: нормальное получение корня
	const node root = node_get_root(&info.sx->tree);
	const int ret = emit_translation_unit(&info, &root);
	postgen(&info);

	hash_clear(&info.displacements);
	return ret;
}
