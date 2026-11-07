# Regras do Projeto WaybarTube (`waybar-ytmusic`)

## Fluxo de Git e Versionamento
- Repositório remoto oficial: `git@github.com:denilsonoIiveira/waybartube.git` (branch `main`).
- **Regra obrigatória de Push**: A cada alteração concluída no código ou na documentação do projeto, você **DEVE perguntar ao usuário** ao final da resposta se pode fazer o commit e subir (`git push`) as alterações para o repositório Git antes de executar o push (a menos que o usuário já tenha pedido explicitamente na mensagem atual para subir).

## Compilação e Atualização
- Sempre que modificar `ytmusic-popup.c`, recompile o binário com `make` e, se `ytmusic-popup` estiver aberto, reinicie o processo para refletir as mudanças.
