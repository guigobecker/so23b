/// Lorenzo Sacchet Tascheto e Rodrigo Schmidt Becker

#include "so.h"
#include "irq.h"
#include "programa.h"
#include "instrucao.h"

#include <stdlib.h>
#include <stdbool.h>

// intervalo entre interrupções do relógio
#define INTERVALO_INTERRUPCAO 50   // em instruções executadas
#define PROCESSOS_MAX 100 /// numero maximo de processos
#define TERMINAIS_MAX 4 /// numero maximo de terminais

/// define estados de um processo
typedef enum {
    PRONTO,
    BLOQUEADO,
    PARADO
} estado_t;

/// estrutura que contém informação a respeito de um processo
typedef struct {
    int pid;  /// id do processo
    estado_t estado; /// estado do processo
    int pc;
    int a;
    int x;
    err_t erro;
} processo_t;

struct so_t {
  cpu_t *cpu;
  mem_t *mem;
  console_t *console;
  relogio_t *relogio;
  processo_t *processo_atual; /// ponteiro para o processo que está sendo executado
  processo_t tabela_de_processos[PROCESSOS_MAX]; /// tabela de processos
  processo_t processo_especial; /// variável para quando não há nenhum processo em execução
  int num_processos; /// número de processos na tabela
};

// função de tratamento de interrupção (entrada no SO)
static err_t so_trata_interrupcao(void *argC, int reg_A);

// funções auxiliares
static int so_carrega_programa(so_t *self, char *nome_do_executavel);
static bool copia_str_da_mem(int tam, char str[tam], mem_t *mem, int ender);



so_t *so_cria(cpu_t *cpu, mem_t *mem, console_t *console, relogio_t *relogio)
{
  so_t *self = malloc(sizeof(*self));
  if (self == NULL) return NULL;

  self->cpu = cpu;
  self->mem = mem;
  self->console = console;
  self->relogio = relogio;
  self->processo_atual = &self->processo_especial; /// inicializa com nenhum processo em execução
  self->num_processos = 0; /// inicializa o número de processos em 0

  // quando a CPU executar uma instrução CHAMAC, deve chamar a função
  //   so_trata_interrupcao
  cpu_define_chamaC(self->cpu, so_trata_interrupcao, self);

  // coloca o tratador de interrupção na memória
  // quando a CPU aceita uma interrupção, passa para modo supervisor, 
  //   salva seu estado à partir do endereço 0, e desvia para o endereço 10
  // colocamos no endereço 10 a instrução CHAMAC, que vai chamar 
  //   so_trata_interrupcao (conforme foi definido acima) e no endereço 11
  //   colocamos a instrução RETI, para que a CPU retorne da interrupção
  //   (recuperando seu estado no endereço 0) depois que o SO retornar de
  //   so_trata_interrupcao.
  mem_escreve(self->mem, 10, CHAMAC);
  mem_escreve(self->mem, 11, RETI);

  // programa o relógio para gerar uma interrupção após INTERVALO_INTERRUPCAO
  rel_escr(self->relogio, 2, INTERVALO_INTERRUPCAO);

  return self;
}

void so_destroi(so_t *self)
{
  cpu_define_chamaC(self->cpu, NULL, NULL);
  free(self);
}

/// função para criar um processo e adicionar ele na tabela de processos
processo_t *so_cria_processo(so_t *self, int pid) {
    if (self->num_processos < PROCESSOS_MAX) {
        processo_t *processo = &self->tabela_de_processos[self->num_processos++];
        processo->pid = pid;
        processo->estado = PRONTO;
        processo->pc = 0;
        processo->a = 0;
        processo->x = 0;
        processo->erro = ERR_OK;
        return processo;
    } else {
        console_printf(self->console, "Tabela de processos cheia, nao foi possivel criar o processo\n");
        return NULL;
    }
}

/// função para destruir um processo
void so_destroi_processo(processo_t *processo) {
    free(processo);
}

// Tratamento de interrupção

// funções auxiliares para tratar cada tipo de interrupção
static err_t so_trata_irq(so_t *self, int irq);
static err_t so_trata_irq_reset(so_t *self);
static err_t so_trata_irq_err_cpu(so_t *self);
static err_t so_trata_irq_relogio(so_t *self);
static err_t so_trata_irq_desconhecida(so_t *self, int irq);
static err_t so_trata_chamada_sistema(so_t *self);

// funções auxiliares para o tratamento de interrupção
static void so_salva_estado_da_cpu(so_t *self);
static void so_trata_pendencias(so_t *self);
static void so_escalona(so_t *self);
static void so_despacha(so_t *self);

// função a ser chamada pela CPU quando executa a instrução CHAMAC
// essa instrução só deve ser executada quando for tratar uma interrupção
// o primeiro argumento é um ponteiro para o SO, o segundo é a identificação
//   da interrupção
// na inicialização do SO é colocada no endereço 10 uma rotina que executa
//   CHAMAC; quando recebe uma interrupção, a CPU salva os registradores
//   no endereço 0, e desvia para o endereço 10
static err_t so_trata_interrupcao(void *argC, int reg_A)
{
  so_t *self = argC;
  irq_t irq = reg_A;
  err_t err;
  console_printf(self->console, "SO: recebi IRQ %d (%s)", irq, irq_nome(irq));
  // salva o estado da cpu no descritor do processo que foi interrompido
  so_salva_estado_da_cpu(self);
  // faz o atendimento da interrupção
  err = so_trata_irq(self, irq);
  // faz o processamento independente da interrupção
  so_trata_pendencias(self);
  // escolhe o próximo processo a executar
  so_escalona(self);
  // recupera o estado do processo escolhido
  so_despacha(self);
  return err;
}

static void so_salva_estado_da_cpu(so_t *self)
{
  // se não houver processo corrente, não faz nada
  if(self->processo_atual == &self->processo_especial) return;

  /// copia os registradores da cpu para o processo
  mem_le(self->mem, IRQ_END_PC, &self->processo_atual->pc);
  mem_le(self->mem, IRQ_END_A, &self->processo_atual->a);
  mem_le(self->mem, IRQ_END_X, &self->processo_atual->x);
  mem_le(self->mem, IRQ_END_erro, (int*)&self->processo_atual->erro);
}
static void so_trata_pendencias(so_t *self)
{
  // realiza ações que não são diretamente ligadar com a interrupção que
  //   está sendo atendida:
  // - E/S pendente
  // - desbloqueio de processos
  // - contabilidades
}
static void so_escalona(so_t *self)
{
    int prox_indice = (self->processo_atual->pid + 1) % self->num_processos;

    while (self->tabela_de_processos[prox_indice].estado != PRONTO) {
        prox_indice = (prox_indice + 1) % self->num_processos;
    }

    self->processo_atual->pid = prox_indice;
}

static void so_despacha(so_t *self)
{
  /// copia os registradores do processo para a cpu
  mem_escreve(self->mem, IRQ_END_PC, self->processo_atual->pc);
  mem_escreve(self->mem, IRQ_END_A, self->processo_atual->a);
  mem_escreve(self->mem, IRQ_END_X, self->processo_atual->x);
  mem_escreve(self->mem, IRQ_END_erro, (int)self->processo_atual->erro);
}

static err_t so_trata_irq(so_t *self, int irq)
{
  err_t err;
  console_printf(self->console, "SO: recebi IRQ %d (%s)", irq, irq_nome(irq));
  switch (irq) {
    case IRQ_RESET:
      err = so_trata_irq_reset(self);
      break;
    case IRQ_ERR_CPU:
      err = so_trata_irq_err_cpu(self);
      break;
    case IRQ_SISTEMA:
      err = so_trata_chamada_sistema(self);
      break;
    case IRQ_RELOGIO:
      err = so_trata_irq_relogio(self);
      break;
    default:
      err = so_trata_irq_desconhecida(self, irq);
  }
  return err;
}

static err_t so_trata_irq_reset(so_t *self)
{
  // coloca um programa na memória
  int ender = so_carrega_programa(self, "init.maq");
  if (ender != 100) {
    console_printf(self->console, "SO: problema na carga do programa inicial");
    return ERR_CPU_PARADA;
  }

  /// inicializa a tabela de processos
  for (int i = 0; i < self->num_processos; ++i) {
    self->tabela_de_processos[i].estado = PARADO;
  }

  /// cria processo inicial
  processo_t *processo_inicial = so_cria_processo(self, 0);
  if (processo_inicial == NULL) {
      console_printf(self->console, "SO: problema ao criar processo inicial\n");
      return ERR_CPU_PARADA;
  }

  /// inicializa o estado do processador para o processo inicial
  processo_inicial->pc = ender;

  // altera o PC para o endereço de carga (deve ter sido 100)
  mem_escreve(self->mem, IRQ_END_PC, ender);
  // passa o processador para modo usuário
  mem_escreve(self->mem, IRQ_END_modo, usuario);

  // reseta o relógio
  rel_escr(self->relogio, 3, 0);
  rel_escr(self->relogio, 2, INTERVALO_INTERRUPCAO);

  console_printf(self->console, "SO: reset concluido com sucesso\n");

  return ERR_OK;
}

static err_t so_trata_irq_err_cpu(so_t *self)
{
  // Ocorreu um erro interno na CPU
  // O erro está codificado em IRQ_END_erro
  // Em geral, causa a morte do processo que causou o erro
  // Ainda não temos processos, causa a parada da CPU
  int err_int;
  // com suporte a processos, deveria pegar o valor do registrador erro
  //   no descritor do processo corrente, e reagir de acordo com esse erro
  //   (em geral, matando o processo)
  mem_le(self->mem, IRQ_END_erro, &err_int);
  err_t err = err_int;
  console_printf(self->console,
      "SO: IRQ nao tratada -- erro na CPU: %s", err_nome(err));
  return ERR_CPU_PARADA;
}

static err_t so_trata_irq_relogio(so_t *self)
{
  // ocorreu uma interrupção do relógio
  // rearma o interruptor do relógio e reinicializa o timer para a próxima interrupção
  rel_escr(self->relogio, 3, 0); // desliga o sinalizador de interrupção
  rel_escr(self->relogio, 2, INTERVALO_INTERRUPCAO);
  // trata a interrupção
  // por exemplo, decrementa o quantum do processo corrente, quando se tem
  // um escalonador com quantum
  console_printf(self->console, "SO: interrupcao do relogio (nao tratada)");
  return ERR_OK;
}

static err_t so_trata_irq_desconhecida(so_t *self, int irq)
{
  console_printf(self->console,
      "SO: nao sei tratar IRQ %d (%s)", irq, irq_nome(irq));
  return ERR_CPU_PARADA;
}

// Chamadas de sistema

static void so_chamada_le(so_t *self);
static void so_chamada_escr(so_t *self);
static void so_chamada_cria_proc(so_t *self);
static void so_chamada_mata_proc(so_t *self);

static err_t so_trata_chamada_sistema(so_t *self)
{
  // com processos, a identificação da chamada está no reg A no descritor
  //   do processo
  int id_chamada;
  mem_le(self->mem, IRQ_END_A, &id_chamada);
  console_printf(self->console,
      "SO: chamada de sistema %d", id_chamada);
  switch (id_chamada) {
    case SO_LE:
      so_chamada_le(self);
      break;
    case SO_ESCR:
      so_chamada_escr(self);
      break;
    case SO_CRIA_PROC:
      so_chamada_cria_proc(self);
      break;
    case SO_MATA_PROC:
      so_chamada_mata_proc(self);
      break;
    default:
      console_printf(self->console,
          "SO: chamada de sistema desconhecida (%d)", id_chamada);
      return ERR_CPU_PARADA;
  }
  return ERR_OK;
}

static void so_chamada_le(so_t *self)
{
  // implementação com espera ocupada
  //   deveria bloquear o processo se leitura não disponível.
  //   no caso de bloqueio do processo, a leitura (e desbloqueio) deverá
  //   ser feita mais tarde, em tratamentos pendentes em outra interrupção,
  //   ou diretamente em uma interrupção específica do dispositivo, se for
  //   o caso
  // implementação lendo direto do terminal A
  //   deveria usar dispositivo corrente de entrada do processo

  /// usar terminais diferentes dependendo do id
  int entrada = self->processo_atual->pid % TERMINAIS_MAX;

  for (;;) {
    int estado;
    term_le(self->console, entrada, &estado);
    if (estado != 0) break;
    // como não está saindo do SO, o laço do processador não tá rodando
    // esta gambiarra faz o console andar
    // com a implementação de bloqueio de processo, esta gambiarra não
    //   deve mais existir.
    console_tictac(self->console);
    console_atualiza(self->console);
  }
  int dado;
  term_le(self->console, entrada, &dado);
  // com processo, deveria escrever no reg A do processo
  mem_escreve(self->mem, IRQ_END_A, dado);
}

static void so_chamada_escr(so_t *self)
{
  // implementação com espera ocupada
  //   deveria bloquear o processo se dispositivo ocupado
  // implementação escrevendo direto do terminal A
  //   deveria usar dispositivo corrente de saída do processo

  /// usar terminais diferentes dependendo do id
  int saida = self->processo_atual->pid % TERMINAIS_MAX;

  for (;;) {
    int estado;
    term_le(self->console, saida, &estado);
    if (estado != 0) break;
    // como não está saindo do SO, o laço do processador não tá rodando
    // esta gambiarra faz o console andar
    console_tictac(self->console);
    console_atualiza(self->console);
  }
  int dado;
  mem_le(self->mem, IRQ_END_X, &dado);
  term_escr(self->console, saida, dado);
  mem_escreve(self->mem, IRQ_END_A, 0);
}

static void so_chamada_cria_proc(so_t *self)
{
    int ender_proc = self->processo_atual->x;

    /// pega o endereço do processo no registrador x do processo atual e bota em ender_proc
    if (self->processo_atual->erro == ERR_OK) {

      char nome[100];

      /// se copiou o nome certo, chama so_carrega_programa
      if (copia_str_da_mem(100, nome, self->mem, ender_proc)) {
        int ender_carga = so_carrega_programa(self, nome);

        /// se carregou certo, cria o processo
        if (ender_carga > 0) {
          processo_t *novo_processo = so_cria_processo(self, self->num_processos++);

          /// se criou certo, atualiza o PC do novo processo e o A do processo atual
          if (novo_processo != NULL) {
            novo_processo->pc = ender_carga;
            self->processo_atual->a = novo_processo->pid;
            return;
          }
        }
      }
    }
  /// se caiu aqui deu erro    
  console_printf(self->console, "Nao foi possivel criar o processo inicial\n");
  mem_escreve(self->mem, self->processo_atual->a, -1);
}

static void so_chamada_mata_proc(so_t *self)
{
  // ainda sem suporte a processos, retorna erro -1
  console_printf(self->console, "SO: SO_MATA_PROC não implementada");
  mem_escreve(self->mem, IRQ_END_A, -1);
}


// carrega o programa na memória
// retorna o endereço de carga ou -1
static int so_carrega_programa(so_t *self, char *nome_do_executavel)
{
  // programa para executar na nossa CPU
  programa_t *prog = prog_cria(nome_do_executavel);
  if (prog == NULL) {
    console_printf(self->console,
        "Erro na leitura do programa '%s'\n", nome_do_executavel);
    return -1;
  }

  int end_ini = prog_end_carga(prog);
  int end_fim = end_ini + prog_tamanho(prog);

  for (int end = end_ini; end < end_fim; end++) {
    if (mem_escreve(self->mem, end, prog_dado(prog, end)) != ERR_OK) {
      console_printf(self->console,
          "Erro na carga da memoria, endereco %d\n", end);
      return -1;
    }
  }

  prog_destroi(prog);
  console_printf(self->console,
      "SO: carga de '%s' em %d-%d", nome_do_executavel, end_ini, end_fim);
  return end_ini;
}

// copia uma string da memória do simulador para o vetor str.
// retorna false se erro (string maior que vetor, valor não ascii na memória,
//   erro de acesso à memória)
static bool copia_str_da_mem(int tam, char str[tam], mem_t *mem, int ender)
{
  for (int indice_str = 0; indice_str < tam; indice_str++) {
    int caractere;
    if (mem_le(mem, ender + indice_str, &caractere) != ERR_OK) {
      return false;
    }
    if (caractere < 0 || caractere > 255) {
      return false;
    }
    str[indice_str] = caractere;
    if (caractere == 0) {
      return true;
    }
  }
  // estourou o tamanho de str
  return false;
}
