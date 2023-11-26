#include "err.h"

static char *nomes[N_ERR] = {
  [ERR_OK]         = "OK",
  [ERR_CPU_PARADA] = "CPU parada",
  [ERR_INSTR_INV]  = "Instrucao invalida",
  [ERR_END_INV]    = "Endereco invalido",
  [ERR_OP_INV]     = "Operacao invalida",
  [ERR_DISP_INV]   = "Dispositivo invalido",
  [ERR_OCUP]       = "Dispositivo ocupado",
  [ERR_INSTR_PRIV] = "Instrucao privilegiada",
};

// retorna o nome de erro
char *err_nome(err_t err)
{
  if (err < ERR_OK || err >= N_ERR) return "DESCONHECIDO";
  return nomes[err];
}
