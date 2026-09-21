# Contexto — mecanismo de classificação ativa de IPC-class (branch `shadow_classification`)

Este arquivo descreve o mecanismo como ele existe hoje, não um log de sessões. Os comentários no código foram deliberadamente reduzidos ao mínimo (uma linha, quando muito, apontando pra cá); toda a razão de ser de cada peça está aqui.

## 1. O problema que isso resolve

Intel Thread Director (ITD) só classifica uma task enquanto ela roda num P-core. Uma task que nasce e vive inteira num E-core nunca é classificada, fica presa no score neutro de fallback pra sempre, e é invisível pra qualquer decisão de posicionamento baseada em classe — exatamente a task que mais se beneficiaria de ser movida.

## 2. O mecanismo: shadow fork ativo

Pra uma task-alvo X residente em E-core: cria-se um clone COW descartável ("shadow") de X, pina-se esse shadow num P-core dedicado e isolado, deixa-se o hardware classificá-lo, copia-se a classe resultante de volta pra X, e destrói-se o shadow. X nunca sai do E-core onde estava; o shadow é que "empresta" o P-core.

O shadow nunca pode produzir efeito colateral externo: ele carrega `SYSCALL_WORK_IPCC_SHADOW`, e sua primeira tentativa de syscall o mata (`syscall_trace_enter()` → `ipcc_shadow_syscall_denied()`), antes da syscall executar.

### Arquivos e o que cada um faz

- **`kernel/fork.c`**: `shadow_kernel_clone()`/`shadow_copy_process()` — o fork em si. `shadow_defang_shared_mappings()`/`shadow_protect_target_shared()` — a parte de memória compartilhada (seção 6).
- **`arch/x86/kernel/sched_ipcc_classifier.c`**: todo o ciclo de vida — fila round-robin por E-core, o kthread `ipcc-reaper`, o limitador de taxa, o dwell.
- **`arch/x86/kernel/sched_ipcc.c`**: o caminho de classificação real via HFI (debounce, weight vector) e o desvio que dá ao shadow uma leitura única não filtrada.
- **`kernel/sched/fair.c` + `kernel/sched/sched.h`**: consumo da classe pelo load balancer (seção 4).
- **`mm/ipcc_stash.c` + `include/linux/ipcc_stash.h`**: mecanismo de COW deferido pra memória compartilhada (seção 7), opcional, desligado por padrão.

## 3. Ciclo de vida de um shadow

1. **Gatilho**: `sched_tick()` (`kernel/sched/core.c`) chama `arch_update_ipcc(rq->curr)` a cada tick de qualquer task. `intel_update_ipcc()` (`sched_ipcc.c`) detecta que a task está num E-core e repassa pra `ipcc_classify_tick()`.
2. **Elegibilidade**: `ipcc_classify_eligible()` exige `mm` real (não kthread), single-threaded (`get_nr_threads()==1` — `CLONE_VM` quebraria o isolamento COW), sem shadow já `SCHEDULED`/`DELIVERED` pra esse alvo, e lag desde o último turno ≥ `IPCC_MIN_LAG_MS` (1ms — ver seção 3.1 sobre por que esse valor).
3. **Submissão**: `ipcc_classify_submit()` instala a task como candidata pro slot do seu E-core (um slot por E-core, não uma fila compartilhada — round-robin dá a cada E-core um turno por volta independente de quão "quente" outro core esteja), evictando quem estava lá antes, e enfileira um `task_work` na própria task.
4. **Fork**: quando a task volta pra userspace, seu `task_work` (`ipcc_shadow_fork_work()`) roda `shadow_kernel_clone()`. O shadow volta **parado** (`TASK_NEW`, pinado no core classificador, nunca acordado ainda) — só um turno pode estar de fato rodando no core classificador a qualquer momento (ITD classifica por-core, dois shadows simultâneos dariam leitura ruidosa).
5. **Dwell**: o `ipcc-reaper` (thread único, round-robin pelos slots) pega o shadow pronto e chama `ipcc_classify_dwell()`, que finalmente acorda o shadow (`wake_up_new_task()`) e espera até: o shadow morrer (syscall trap), o shadow confirmar uma classe (`ipcc_shadow_confirmed()`), ou o orçamento `IPCC_SHADOW_DWELL_MS` (10ms) estourar.
6. **Copy-back**: se o shadow chegou a produzir uma classe, ela e o vetor de peso (EWMA) são copiados de volta pro alvo (`ipcc_blend_class_weight()` — aplica k passos de EWMA, k = quantas leituras confirmadas o shadow acumulou).
7. **Morte**: `send_sig(SIGKILL, shadow, 1)`; reaping automático porque o reaper ignora `SIGCHLD`.

### 3.1 Por que `IPCC_MIN_LAG_MS = 1`

Testado sob contenção real (4 processos fazendo syscall em loop apertado + 1 alvo compute-bound): mesmo o alvo recebendo só ~7% dos turnos, isso ainda dava ~100 turnos/s — suficiente pra convergir o vetor de peso em <150ms. Vazão agregada subiu ~4-5x sobre o piso anterior de 200ms. Não há fila por-slot além de 1 pendente, então ser "atropelado" não significa fome real — só perde a corrida imediata por um slot que acabou de abrir.

### 3.2 Máquina de estados `ipcc_shadow_status`

Campo em `task_struct` (`include/linux/sched.h`), 4 valores: `NONE`, `SCHEDULED` (task_work enfileirado, fork ainda não rodou), `DELIVERED` (shadow vivo, em dwell), `EVICTED`. Existe pra garantir **no máximo um shadow vivo ou em voo por alvo**. `ipcc_classify_eligible()`/`ipcc_classify_submit()` recusam tanto `SCHEDULED` quanto `DELIVERED` — recusar só `DELIVERED` deixaria um buraco real: se o alvo migra de E-core antes do seu `fork_work` pendente rodar (o `IPCC_MIN_LAG_MS` de 1ms é curto o bastante pra isso acontecer), um segundo `ipcc_classify_submit()` passaria, e como `ipcc_shadow_req.state` é por-requisição (não por-alvo), os dois poderiam "vencer" seu próprio CAS independentemente — dois shadows vivos do mesmo alvo ao mesmo tempo.

A transição é resolvida via CAS num único `atomic_t` (`ipcc_shadow_req.state`, 0→`EVICTED`/`DELIVERED`) entre `ipcc_shadow_fork_work()` e `ipcc_evict()`: quem perde a corrida é quem tem dados stale-ou-ausentes, e por isso é quem faz a limpeza (o outro lado já tem, ou vai ter, `->shadow`/`->error` válidos).

Não existe (e não precisa existir) um ponteiro persistente pro `task_struct` do shadow no alvo — só o enum. A referência contada real vive inteira em `ipcc_shadow_req.shadow`, e mais nada precisa dela: nenhum código lê o ponteiro pra decidir algo, só pra saber "existe um shadow agora" — que o enum já responde sozinho.

## 4. Consumo da classe pelo load balancer

`ipcc_weighted_score(p, cpu)` (`kernel/sched/sched.h`) dá o score esperado de `p` em `cpu`, ponderado pelo histórico recente de mistura de classes (`p->ipcc_class_weight[]`, um vetor EWMA por classe) — não só a classe atualmente confirmada. Task sem pesos acumulados devolve o baseline da própria cpu. Isso alimenta:

- **`ipcc_misfit_weight()`** (`fair.c`): decide se uma task já está "misfit" no core em que está, comparando o score alcançável no melhor P-core (`ipcc_best_pcore_cpu`, latched via score de pico HFI + prioridade ITMT como desempate) contra o score no core atual, elevado a um expoente que cresce conforme os P-cores ficam mais escassos (`ipcc_system_load`, [1, `IPCC_MAX_EXP`]). Usa razão de scores, não o baseline.
- **`wake_affine_weight()`, `sched_balance_find_dst_group_cpu()`**: usam `cpu_load_ipcc()`/`task_h_load_ipcc()` — importante porque o resultado de um cenário 1×1 é dominado pela colocação inicial no wake, não só pelo balanceamento periódico.
- **`update_sg_lb_stats()`/`update_sg_wakeup_stats()`/`sched_balance_find_src_rq()`**: `group_load`/`group_util`/`group_runnable` via `cpu_load_ipcc()`/`cpu_util_ipcc()`/`cpu_runnable_ipcc()` (e as variantes `_without` pro caso de wake).

### 4.1 Baseline por tipo de cpu

`apply_ipcc_weight(p, cpu, x) = x × baseline(cpu) / ipcc_weighted_score(p, cpu)`. O `baseline(cpu)` (`intel_hfi_get_ipcc_baseline(cpu)`, `drivers/thermal/intel/intel_hfi.c`) é a **média de `perf_cap` sobre as classes da tabela HFI daquela própria cpu**, calculada no mesmo laço de `set_hfi_ipcc_scores()` que já preenche os scores e guardada num `int __percpu`, sob o mesmo `hfi_ipcc_seqcount`. Nesta máquina: 66 nos P-cores (59/69/92/46) e 34 nos E-cores (34 em todas as classes), confirmado em `dmesg | grep "IPCC BASELINE"`.

Por que por cpu e não global: o baseline anterior era uma constante congelada (média das classes de um P-core de referência, ~66). Isso multiplicava por ~1,96 toda carga num E-core, e a capacidade do E-core (590 contra 1024) já expressa a razão média entre os cores — o mesmo fato entrava duas vezes, e o LB via uma task cls2 no E como ~3,5× mais pesada por unidade de capacidade quando o ganho real é ~2×. Com o baseline por cpu, a capacidade carrega a razão média e o peso carrega **só o desvio da classe** em relação à média daquele tipo de core:

| | E-core | P-core |
|---|---|---|
| peso, qualquer classe | 1,00 | cls1 1,13 · cls2 0,96 · cls3 0,72 · cls4 1,45 |

A tabela HFI é populada uma vez no boot nesta máquina (uma só ocorrência de `IPCC SCORE: cpu=0 classe=0` no dmesg), então na prática o baseline é constante, embora o código o recalcule a cada atualização da tabela. `ipcc_misfit_weight()` não usa baseline.

### 4.2 Carga ponderada derivada, sem agregado

`cpu_load_ipcc()`, `task_h_load_ipcc()` e `cpu_load_without_ipcc()` derivam a carga ponderada da carga **crua** (`cpu_load()`, `task_h_load()`, `cpu_load_without()`) ponderada pela classe de `rq->curr` (ou da própria task), o mesmo padrão de `cpu_util_ipcc()`/`cpu_runnable_ipcc()`.

Isso substituiu a leitura de `rq->cfs.avg.load_avg_ipcc`, um agregado mantido por attach/detach/tick que **nunca decaía**: só somava no attach, subtraía no detach e era atualizado no tick apenas para o `curr`, então cada task bloqueada deixava seu último valor congelado. Medido: cpus ociosas (`nr=0`) com `group_load` de ~200 mil contra ~10 depois da correção, o que deixava todos os grupos com carga quase igual e fazia `sched_balance_find_src_group()` devolver NULL em ~99% das tentativas (2 migrações em 14s com 6 tasks contra 1). O agregado continua sendo escrito mas ninguém o lê.

### Lacuna: sem evicção (tratada na seção 5)

Nenhum dos mecanismos de balanceamento (misfit por capacidade, `asym_packing`) faz preempção — os dois só agem quando o core de destino está ocioso ou prestes a ficar (`env->idle` obrigatório). Uma vez que uma task ocupa o único P-core livre, nada a tira de lá em favor de uma candidata melhor classificada. O load balancer também é estruturalmente cego a carga agregada equilibrada com encaixe de classe desalinhado (uma task de classe alta num E-core, uma de classe baixa num P-core): nenhuma métrica de `group_util`/`group_runnable` enxerga isso.

## 5. Evicção ativa contra ótimos locais

Timer deferrable (`ipcc_swap_timer_fn()`, `kernel/sched/fair.c`, `IPCC_SWAP_PERIOD_MS = 200`) que troca duas tasks entre um P-core e um E-core quando isso melhora o encaixe de classe. Tem dois caminhos, escolhidos pelo que cada cpu tem em fila.

### 5.1 Caminho normal: tasks fora de `curr`

Vale quando os dois lados têm pelo menos 2 tasks. Sorteia um par de cpus, pega `double_rq_lock()` e procura, em `cfs_tasks` dos dois lados, um par de tasks **não em execução** (`!task_on_cpu() && !task_current_donor()`). Move as duas com `move_queued_task_locked()` — a mesma manipulação de fila que `can_migrate_task()` já assume segura sem stopper, sem IPI. Busca **first-fit**: para no primeiro par que passa em `ipcc_swap_pair_worth_it(..., true)`.

**Gate de entrada** (`ipcc_swap_worth_trying()`, antes de qualquer lock): desiste se algum lado tem `cfs.h_nr_runnable < 2` (sem candidata fora de `curr`) ou se a ocupação crua dos dois lados difere além de `sd->imbalance_pct` (117, lido de `per_cpu(sd_llc, ...)`; comparação por ocupação `util_p × cap_e` contra `util_e × cap_p`, porque as capacidades diferem). A troca é 1-por-1 e preserva contagem, então não corrige desequilíbrio de carga; se há um, quem age é o LB, medido na métrica crua e com o limiar dele.

**`ipcc_swap_pair_worth_it()`**: (1) ganho — `score_after` (cada task no lado da outra) precisa superar `score_before` por mais de `IPCC_SWAP_MIN_GAIN_PCT` (5%), somando `ipcc_weighted_score()`; (2) ocupação — simula a util ponderada pós-troca dos dois lados e exige que continuem dentro de `imbalance_pct` entre si. Como `ipcc_weighted_score()` não tem termo de carga, o par inverso tem ganho negativo e é reprovado: o swap não desfaz o próprio swap, e não há ciclo a amortecer.

### 5.2 Caminho de `curr`: uma task por cpu

Com uma task em cada cpu (ou 1 e 0, 0 e 1) essa task **é** o `curr`, o gate do caminho normal reprova e o desalinhamento é um ótimo local estável: cls1 ocupando o P, cls2 no E, sem que o LB veja nada a fazer. Medido: no placement 1+1, cls1 ficou 99,7% do tempo no P por 30s sem swap nenhum.

Mexer em `curr` exige um stopper (é o stopper que a tira de `curr`), então:

- **Portão de tempo**: uma tentativa por `IPCC_CURR_SWAP_HOLDOFF_MS` (1000ms), carimbada na **tentativa** (`ipcc_last_swap`), não só no sucesso; um swap normal aceito também reinicia a contagem. Um flag `ipcc_curr_swap_busy` garante uma tentativa em voo.
- **Varredura** (`ipcc_try_curr_swap()`): percorre todos os pares P×E dos intervalos, filtrando sem lock por `READ_ONCE(nr_running) <= 1`, e chama `ipcc_curr_swap_prepare()` até o primeiro que aprovar (first-fit). Substituiu o sorteio de 1 E-core por disparo, que levava ~8 disparos para achar o par.
- **`ipcc_curr_swap_prepare()`**: sob `double_rq_lock`, exige cpu ativa, `nr_running == 1 && cfs.h_nr_runnable == 1` em cada lado com task (o outro lado precisa estar ocioso), task da classe fair, sem `PF_KTHREAD`/`PF_EXITING`, sem migração desabilitada e com destino em `cpus_ptr`. Para 1 e 1 usa `ipcc_swap_pair_worth_it(..., false)`, **sem** o teste de ocupação: a util de uma task saturando o P (1024) não cabe num E de capacidade 590, e o teste rejeitaria todo swap de tasks CPU-bound, mas com uma task por cpu antes e depois não existe desequilíbrio de contagem para o LB corrigir. Para 1 e 0 e 0 e 1 exige só o mesmo ganho mínimo. Pega referências das tasks.
- **Execução em processo**: `stop_two_cpus()` bloqueia em `wait_for_completion()`, o que é proibido no softirq do timer, e não há variante `nowait` para duas cpus. O `queue_work()` sai depois de soltar os `rq_lock` (acordar o worker sob eles poderia travar) e o work chama `migrate_swap()` (1 e 1) ou `migrate_task_to()` (1 e 0). Ambos existem em `core.c`/`sched.h` também sob `CONFIG_IPC_CLASSES`, mantendo sob NUMA só os tracepoints `*_numa`.
- **Revalidação sem lock** no início do work (`ipcc_curr_swap_still_valid()`): cpu ativa, `nr_running <= 1`, `task_cpu` esperado e sem migração desabilitada. Sem lock de propósito: um lock aqui não seria mais autoritativo, já que é solto antes de `migrate_swap()` dormir. Serve para não pagar dois stoppers quando o estado já mudou. Não é uma correção de segurança: `migrate_disable_switch()` estreita o `cpus_ptr` de uma task com migração desabilitada quando o stopper a preempta, então `migrate_swap_stop()` já devolve `-EAGAIN` pelo teste de afinidade.
- **Falso sucesso**: `migration_cpu_stop()` devolve 0 mesmo quando desiste (`pending == NULL`), então depois de `migrate_task_to()` relê `task_cpu()` e converte em `-EAGAIN` se a task não chegou.

**Como o stopper funciona aqui**: `stop_two_cpus(dst_cpu, src_cpu, ...)` acorda o stopper (classe `stop_sched_class`) das duas cpus, que preempta o `curr` de cada uma; quem chamou (o kworker) só dorme. Os dois stoppers rodam `multi_cpu_stop()`, uma máquina de estados (`PREPARE → DISABLE_IRQ → RUN → EXIT`) em que só a cpu de `active_cpus` (a primeira passada, o E-core) executa `migrate_swap_stop()` e a outra gira com IRQs desabilitadas. É esse custo, dois stoppers e uma cpu girando, que o portão de 1s limita.

### 5.3 Amostragem dos intervalos: `[min,max]` + `step`

O caminho normal sorteia um cpu em `[ipcc_pcore_min, ipcc_pcore_max]` e um em `[ipcc_ecore_min, ipcc_ecore_max]` (`ipcc_pick_range_cpu()`); o caminho de `curr` varre os mesmos intervalos. Limites e `step` são recalculados por `ipcc_latch_best_pcore()`, não a cada troca.

- **Classificação P vs E**: por tipo de CPU real (`ipcc_cpu_is_ecore()`, CPUID `topo.cpu_type`), não por score HFI de pico — P-cores podem ter scores de pico diferentes entre si (favored-core binning).
- **Filtro `HK_TYPE_DOMAIN`**: o intervalo exclui o cpu isolado do classificador, que nunca deve ser alvo. O desempate ITMT de `ipcc_best_pcore_cpu` não usa esse filtro: é só referência de score.
- **`ipcc_pick_classifier_cpu()` escolhe o P-core de maior número** (`arch/x86/kernel/sched_ipcc_classifier.c`), o que mantém o cpu isolado numa ponta do intervalo. Sem isso, excluí-lo abriria um buraco no meio.
- **`step`** (`ipcc_pcore_step`, 1 ou 2): siblings SMT offline intercalados na numeração (`nosmt`, `nosmt=force`). Vem de `__max_threads_per_core` (`ipcc_max_smt_threads()`), fato de topologia parseado de APIC ID antes de `cpu_bootable()`. `cpu_smt_possible()` foi descartado: devolve `false` tanto para hardware sem SMT quanto para `CPU_SMT_FORCE_DISABLED`, não distingue os dois casos. E-cores nunca têm siblings aqui.

### 5.4 Frequência e custo

- **Período de 200ms**: o piso é a janela de IRQ desabilitada com dois `rq_lock` (caminho normal) e `get_actual_cpu_capacity()`, que se move em dezenas de ms. `TIMER_DEFERRABLE` faz o timer não acordar cpu ociosa.
- **Holdoff de 1s no caminho de `curr`**: custo estimado de um swap de `curr` ~1ms combinado (dois stoppers, IPIs e dois caches frios; estimativa, não medido). O par cls1-no-P/cls2-no-E perde ~10% de score agregado (93 contra 103) continuamente, então o ponto de equilíbrio é ~10ms; a 1 swap por segundo o custo fica abaixo de ~0,1%. O 1s é conservador, e algo entre 250 e 500ms provavelmente também serviria.

### 5.5 Resultados medidos (placement, cls2 `rand48` contra cls1 `matrixprod`, N de cada)

| N | resultado |
|---|---|
| 1+1 | cls2 no P 100% em todas as 8 rodadas; 0 a 2 swaps de `curr`, o primeiro entre +0,17s e +0,92s do fork |
| 2+2 | cls2 ocupou o P, cls1 0% no P |
| 3+3 | cls1 0,2 a 0,7% no P; em 90s houve 2 swaps, ambos nos primeiros 8s, e nenhum depois |

Nenhuma falha em nenhuma rodada (`curr failed` = 0). Nas rodadas em que o cls2 caiu no P por sorte no fork não houve swap. Os dados não mostram desfazimento sistemático: as tasks trocadas voltaram ao P na mesma proporção que qualquer outro movimento P→E (61% contra 58%, em 36 workers).

### Limitações conhecidas

- Uma vez corrigido, o estado depende de o LB não movê-lo; o LB gira tasks entre cpus (~1 migração por segundo por task com mais tasks que cpus), e o swap não protege as tasks trocadas.
- O ganho compara a soma de scores sem ponderar por utilização: trocar uma task quase ociosa conta o mesmo que uma CPU-bound.
- `first-fit`: aceita o primeiro par acima de 5% de ganho, não o melhor.
- Tasks com afinidade fixa não são candidatas (`cpus_ptr` cruzado).

## 6. Corrupção de memória compartilhada (Problema A) e o fix `VM_MAYSHARE`

`dup_mmap()` clona o endereço do alvo fielmente, **incluindo** mapeamentos `VM_SHARED` — e `is_cow_mapping()` exclui `VM_SHARED` da proteção COW por definição, então uma VMA compartilhada chega no shadow com PTEs já graváveis, apontando pra mesma página física do alvo real. O shadow, replicando fielmente as instruções do alvo, vira um segundo escritor sem sincronia — indistinguível de uma thread extra descoordenada. Confirmado em hardware: `stress-ng --cpu-method rand48` (mantém seu contador de bogo-ops numa região `MAP_SHARED`) mostrou ~29% dos workers sombreados com corrupção verificada por hash, zero oops (é uma corrida silenciosa, não uma falta).

**Fix**: `shadow_defang_shared_mappings()` (`kernel/fork.c`), chamada uma vez por shadow logo após `copy_mm()`. Pra cada VMA `VM_SHARED|VM_MAYWRITE` (exceto `VM_HUGETLB`, não verificado contra o COW próprio do hugetlb):
1. `wp_shared_mapping_vma()` (`mm/mapping_dirty_helpers.c`) escreve-protege todas as PTEs presentes.
2. Se o stash (seção 7) estiver ativo, `ipcc_stash_arm_vma()` troca as PTEs por um marker que passa a interceptar leitura *e* escrita.
3. `mapping_unmap_writable()` balanceia o `i_mmap_writable` que `dup_mmap()` incrementou — sem isso, o decremento em `__remove_shared_vm_struct()` no teardown seria pulado (ele olha os `vm_flags` *no momento do teardown*, já sem `VM_SHARED`), vazando +1 por shadow forkado pra sempre nesse contador do arquivo real. Isso é o mesmo contador que `mapping_deny_writable()` (`mm/memfd.c`, por trás de `F_SEAL_WRITE`) consulta — inflado, um `memfd_create()` real nunca mais conseguiria selar contra escrita.
4. `vm_flags_clear(vma, VM_SHARED | VM_MAYSHARE)` — **os dois juntos**, não só `VM_SHARED`. Bug real encontrado e corrigido: `mmap()` só seta os dois juntos, nunca um sem o outro, e o gate de `do_wp_page()` (`vma->vm_flags & (VM_SHARED|VM_MAYSHARE)`) foi escrito assumindo esse pareamento. Limpar só `VM_SHARED` deixava `VM_MAYSHARE` sobrevivendo sozinho — um estado que nunca ocorre naturalmente — e isso bastava pra satisfazer o gate de `do_wp_page()` pros folios grandes que `ipcc_stash_arm_pte()` deixa sem marker (splitar um folio grande é cirurgia fora de escopo): a escrita do shadow caía no `wp_page_shared()`/`finish_mkwrite_fault()` de estoque, que reutiliza a página **na física real** — reabrindo a mesma corrupção, só pra páginas grandes. Confirmado em hardware: um shadow de `systemd-journald` bateu esse caminho exato e corrompeu o journal real.

`shadow_protect_target_shared()` é a outra metade: escreve-protege as páginas compartilhadas do **alvo** (não do shadow), só quando o stash está ativo — sem alguém pra consumir o snapshot, seria custo puro. Isso é necessário porque proteger só o shadow não impede o *alvo* de continuar escrevendo sem falta na mesma página física — o shadow, com uma PTE só-leitura, ainda conseguiria **ler** a mutação ao vivo. Medido: oito leituras de `process_vm_readv()` na mesma célula, 8µs de intervalo, oito valores diferentes.

## 7. COW deferido (stash) — `CONFIG_IPC_CLASSES_SHADOW_DEFER_COW`

Desligado por padrão mesmo compilado (`echo 1 > /sys/kernel/debug/ipcc_stash/enabled`). Ideia: hoje, cada escrita do alvo numa página que o shadow também vê custa ao alvo real uma quebra de COW completa (alocar, copiar 4KB, rmap, LRU) — mesmo que o shadow, que morre em menos de 1ms na maioria das vezes, nunca chegue a ler aquela página. O mecanismo inverte quem paga: na primeira escrita do alvo depois do fork, ele guarda o conteúdo antigo numa "stash" (sem entrar em rmap/LRU) e continua escrevendo na página original no lugar; só se o shadow realmente tocar aquele endereço é que a stash vira uma página anônima real no espaço dele.

- **Onde o payload mora**: não na PTE do shadow (escrever na tabela de página de um `mm` alheio de dentro do fault handler do alvo não tem precedente na árvore) — numa lista simples pendurada no `mm_struct` do shadow (`mm->ipcc_stash`), protegida pelo `ipcc_shadows_lock` do **alvo** (alcançado do lado do shadow via `mm->ipcc_shadow_of`, que segura um `mmgrab()`).
- **`ipcc_stash_wp_shared()`** (`mm/memory.c`, hook em `do_wp_page()`): roda no fault de escrita do alvo. Captura o conteúdo atual (ainda é o de antes do fork, é exatamente isso que dispara o trap), oferece pra stash, deixa a escrita prosseguir normalmente no lugar.
- **`ipcc_stash_fault()`** (hook em `handle_pte_marker()`): roda no fault do shadow sobre o marker. Se existe stash pra esse endereço, instala como página anônima privada. Se não existe, cai em `do_pte_missing()` com `FAULT_FLAG_WRITE` forçado — necessário pra garantir `do_cow_fault()` (cópia privada sempre) em vez de `do_read_fault()` (que apontaria direto pra página compartilhada viva, reabrindo a mesma exposição, só adiada pra esse fault).
- **Lacuna conhecida, não fechada**: a cópia em `ipcc_stash_wp_shared()` roda sem lock contra um **outro processo** (não o alvo) escrevendo a mesma página compartilhada ao mesmo tempo — pode capturar um valor parcialmente atualizado. É inconsistência de snapshot, não falta de memory-safety; não se manifesta com um único escritor por página (o cenário validado).
- **Métricas** (`/sys/kernel/debug/ipcc_stash/stats`): `created`/`consumed`/`discarded` e `discard_ratio_pct` — a fração de snapshots preservados e nunca lidos é o que diz se o mecanismo vale a pena (alto = a maioria dos shadows nunca sobrevive pra usar a stash, exatamente o caso comum). Testado sob contenção pesada (8 pares escritor/leitor, um por E-core, 30s, ~940 mil verificações): zero corrupção, ~30 mil capturas de stash, 100% de descarte (esperado — shadows raramente sobrevivem tempo suficiente pra reexecutar até tocar o endereço).

## 8. O crash que travava a máquina inteira (não relacionado à memória)

Sintoma: sistema inteiro parava de responder, sem panic, exigindo desligamento forçado. Não tinha relação com nada da seção 6/7 — confirmado desligando `shadow_defang_shared_mappings()` inteira (early return) e reproduzindo o travamento mesmo assim.

Causa raiz, em `shadow_copy_process()` (existe desde a versão original do mecanismo, não é regressão recente):
- `p->fs = NULL` — o shadow nasce sem `fs_struct`, na suposição de que `->fs` só é alcançável via syscall, e o shadow morre na primeira tentativa.
- `CLONE_CLEAR_SIGHAND` no `kernel_clone_args` — reseta os handlers de sinal herdados pro padrão.

Essas duas suposições se combinam mal com um workload real: runtimes como V8/Node usam SIGSEGV **de propósito e recuperável** (bounds-check de WASM via guard page, probe de stack-overflow) — o processo real tem handler instalado e se recupera silenciosamente (`unhandled_signal()`, `kernel/signal.c`, nem imprime a mensagem de log quando há handler de verdade). O shadow, clonado no meio da execução do mesmo código mas **sem** esse handler, transforma a mesma instrução recuperável em fatal de verdade.

Sinal fatal + coredumpable → `get_signal()` → `vfs_coredump()` → `elf_core_dump()` → `fill_files_note()` → `d_path()` (`fs/d_path.c:286`) → `get_fs_root_rcu(current->fs, ...)` **sem checar NULL** → dereference de `NULL+4` (offset de `fs->seq` em `struct fs_struct`) → oops. Esse oops acontece **dentro do `rcu_read_lock()` que o próprio `d_path()` toma** (`fs/d_path.c:285`) — o unwind pula o `rcu_read_unlock()` correspondente, a contagem de aninhamento de RCU dessa task nunca mais volta, e todo período de graça de RCU síncrono do sistema (usado por boa parte do kernel) fica esperando por ela pra sempre. É isso que trava a máquina inteira, não só a task.

**Fix**: `set_dumpable(p->mm, SUID_DUMP_DISABLE)` logo após `copy_mm()` em `shadow_copy_process()`. Faz `coredump_skip()` (`fs/coredump.c`) retornar antes de `vfs_coredump()` fazer qualquer coisa — o shadow nunca mais chega perto de `elf_core_dump()`/`d_path()`, não importa qual sinal fatal ele receba. Validado ao vivo: o `claude` (processo real, com handler de SIGSEGV confirmado via `/proc/pid/status` `SigCgt`) segfaultou duas vezes durante um teste de 30s com o fix aplicado, e o sistema seguiu respondendo normalmente — só o processo (quase certamente um shadow dele, não o processo real, já que o real tem handler e não geraria a mensagem de log) morreu.

Um `p->signal->rlim[RLIMIT_CORE].rlim_cur = 0` já existia antes como tentativa de mitigar isso — insuficiente sozinho, porque `coredump_skip()` não olha `RLIMIT_CORE`, só o tamanho do arquivo depois de já ter alcançado `elf_core_dump()`. Mantido como reforço; `set_dumpable()` é quem efetivamente fecha o caminho.

## 9. Estado do build

- `CONFIG_IPC_CLASSES_ACTIVE_CLASSIFIER=y` — mecanismo base sempre ativo.
- `CONFIG_IPC_CLASSES_SHADOW_DEFER_COW` — controla se `mm/ipcc_stash.c` é compilado; requer também `#include <linux/swap.h>` antes de `<linux/swapops.h>` nesse arquivo (define `MAX_SWAPFILES_SHIFT`/`SWP_*` que `swapops.h` só usa, não define).
- Runtime toggle independente do Kconfig: `/sys/kernel/debug/ipcc_stash/enabled` (0 por padrão mesmo com o Kconfig ligado).
- `/sys/kernel/debug/ipcc_classify`: gatilho manual, escreve um pid pra submeter à classificação (mesma fila do caminho por tick).
- `migrate_swap()`/`migrate_task_to()` (`core.c`, `sched.h`) compilam com `CONFIG_NUMA_BALANCING || CONFIG_IPC_CLASSES`; o caminho de `curr` da seção 5.2 depende delas.
- Diagnóstico ativo no código: `trace_printk("ipcc-swap: ...")` no swap e `trace_printk("lb: ...")` no balanceador padrão (`IPCC_LB_TRACE`, pula newidle). O segundo gera volume alto; retirar com `//` antes de medir desempenho.

## 10. Notas operacionais

- Debugfs (`/sys/kernel/debug`) é montado 0700 root-only nesta máquina — precisa de `sudo` pra ler/escrever qualquer coisa sob `ipcc_stash`/`ipcc_classify`.
- E-cores nesta máquina: cpu4-11 (8). P-cores: cpu0, cpu2 (cpu1/cpu3 são os SMT siblings, offline via `nosmt`).
- `nosmt` (sem `=force`) preserva capacidade uniforme mas ainda tira os siblings SMT do escalonamento — suficiente pra manter a classificação precisa (`sched_smt_siblings_idle()` trata sibling offline como idle) sem desistir da capacidade real assimétrica.
