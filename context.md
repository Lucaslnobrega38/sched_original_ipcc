# Contexto — branch `shadow_classification`

Estado do mecanismo de classificação ativa de IPC-class no dia da última rodada de testes. Este arquivo existe pra alguém (inclusive eu mesmo, depois) retomar sem precisar re-derivar tudo.

## O mecanismo, resumido

Intel Thread Director só classifica tasks rodando em P-core; E-cores nunca produzem classificação válida. O mecanismo aqui cria, periodicamente, um clone COW descartável ("shadow") de uma task residente em E-core, o pina num P-core isolado dedicado só a isso, deixa o hardware classificá-lo, copia a classe de volta pra task real, e mata o shadow — sem nunca tirar a task real do E-core onde estava.

Peças principais:
- `kernel/fork.c`: `shadow_kernel_clone()`/`shadow_copy_process()` — fork enxuto, shadow nasce parado (`TASK_NEW`) já pinado no core classificador, só é acordado na hora do seu turno.
- `arch/x86/kernel/sched_ipcc_classifier.c`: fila round-robin por E-core (`ipcc_queue[]`), reaper single-thread (`ipcc-reaper`), limitador de taxa por alvo (`IPCC_MIN_LAG_MS`), dwell que termina assim que o shadow morre ou confirma uma classe.
- `arch/x86/kernel/sched_ipcc.c`: caminho de classificação real (P-core, com debounce) e o desvio que dá ao shadow uma leitura única e não filtrada (`intel_classify_ipcc_final()` + `ipcc_shadow_confirmed()`).
- `kernel/sched/fair.c`: consumo da classe pelo load balancer — `ipcc_weighted_score()`, `apply_ipcc_weight()`, `ipcc_misfit_weight()`, e a EWMA por classe (`ipcc_class_weight[]`) em `include/linux/sched.h`.

## Validado nesta rodada

- **`IPCC_MIN_LAG_MS` baixado de 200ms pra 1ms** (bate com 1 tick a HZ=1000). Testado sob pressão real: 4 processos "efêmeros" (syscall em loop apertado) dominaram 93% dos turnos do classificador, mas o alvo compute-bound real ainda recebeu ~101 turnos/s — mais que suficiente pra convergir a EWMA em <150ms. Vazão agregada subiu ~4-5x sobre o piso de 200ms. Sem fome real medida.
- **Mecanismo "confirmação única" reconstruído** (`ipcc_shadow_confirmed_at`, `ipcc_shadow_confirmed()`): o shadow recebe exatamente uma leitura crua, sem debounce, e o dwell termina imediatamente — não espera mais o teto de `IPCC_SHADOW_DWELL_MS` pra alvos que nunca fazem syscall. Isso tinha sido implementado e validado numa rodada anterior desta mesma sessão, mas não estava commitado — o squash final (`ccfe64e01`) foi tirado de um estado anterior a essa implementação. Reconstruído agora, fielmente.
- **`asym_packing` e capacidade de hardware assimétrica são mutuamente exclusivos no kernel vanilla** — `hybrid_init_cpu_capacity_scaling()` liga uma coisa e desliga a outra, nunca as duas juntas. Com `nosmt=force`, a capacidade real assimétrica (P=1024, E~590) liga e o ITMT/`asym_packing` desliga automaticamente. Confirmado via dmesg, não suposição.
- **Nenhum dos dois mecanismos de balanceamento (misfit por capacidade, `asym_packing`) faz preempção.** Os dois só agem quando o P-core de destino está ocioso ou prestes a ficar (`env->idle` obrigatório em `update_sg_lb_stats`/`sched_group_asym`). Uma vez que uma task ocupa o único P-core livre, nada a tira de lá — o resultado de quem "ganha" um cenário 1-contra-1 é dominado pela sorte da colocação inicial no fork, não pela classe.
- **Teste de placement 1×1 (`rand48` vs `div16`) em 25 repetições**: `rand48` venceu 14/25 (56%) — direção certa (razão real de hardware 69/34≈2.03x contra 59/34≈1.74x), mas `P(≥14/25 só por sorte) = 34.5%` — não é estatisticamente distinguível de moeda justa nessa amostra. Confirma a limitação acima: com só 1 P-core disponível, o efeito de classe é pequeno demais pra emergir sem MUITAS repetições ou um par de classes com razão mais separada (ex. genuinamente vetorial, 92/34≈2.71x).
- **Correção adicional aplicada hoje**: `wake_affine_weight()`, `sched_balance_find_dst_group_cpu()`, e os dois `update_sg_*_stats` (`group_util`/`group_runnable`) foram trazidos pra usar as variantes ponderadas por ipcc (`cpu_load_ipcc`, `cpu_util_ipcc`, `cpu_runnable_ipcc` e as versões `_without`). Antes, só `group_load`/`ipcc_misfit_weight` eram cientes de classe — a decisão de **onde acordar uma task nova** (que é o que decide o resultado do cenário 1×1 acima) estava cega a classe. Ainda não retestado com essa correção.

## Não resolvido / lacuna conhecida

Nenhum mecanismo ativo de **evicção**: se uma task mal-classificada já está ocupando o único P-core, nada a tira de lá em favor de uma candidata melhor. Ideia desenhada (não implementada): um timer dedicado e barato, tipo power-of-two-choices — sorteia 1 P-core e 1 E-core, compara `ipcc_weighted_score()` de quem está rodando em cada um via `rq->curr`, troca os dois com `double_rq_lock()` se compensar. Não precisa de core ocioso pra agir, ao contrário do misfit/`asym_packing`.

## Estado não commitado nesta branch

`arch/x86/kernel/sched_ipcc.c`, `arch/x86/kernel/sched_ipcc_classifier.c`, `include/linux/sched.h`, `kernel/sched/fair.c` têm mudanças locais não commitadas além de `ccfe64e01` — cobrem tudo listado acima ("Validado nesta rodada"). `drivers/thermal/intel/intel_hfi.c` e `drivers/cpufreq/intel_pstate.c` **não** foram tocados nesta branch (ficaram intocados de propósito, já que o teste atual usa a tabela real de hardware e `nosmt=force` pra capacidade assimétrica real, não a tabela sintética espelhada+incentivada explorada em outra rodada).

## Nota operacional

Muitos reboots seguidos de kernel de teste corromperam o journal do systemd em algum ponto (`journalctl --verify` mostrava "Bad message"/"Object number mismatch" — sintoma clássico de gravação cortada por desligamento sujo). Resolvido com `journalctl --vacuum-time=1s` + restart do serviço; pode voltar a acontecer se um boot travar/for interrompido à força de novo.
