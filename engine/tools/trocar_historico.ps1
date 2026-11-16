# Depois da limpeza do histórico (16/11/2026): atualiza esta pasta para o histórico novo e move as tags das
# versões para os commits novos (o GitHub as guarda; esta sessão na nuvem não pode enviar tags).
# Uso (na pasta engine, uma vez só): powershell -ExecutionPolicy Bypass -File tools\trocar_historico.ps1
git fetch origin
git checkout -q ccr-5423dd89-y3br6e
git reset --hard origin/ccr-5423dd89-y3br6e
git push -f origin 4fcaf05ad7446e36c141a494d6f40474bd0d89bf:refs/tags/v0.1.282
git push -f origin 849e34f1363840176c8784cc864d7dc440fcad94:refs/tags/v0.1.289
git push -f origin c844f2acb3b8c364e460a28f6163b77e98f78e49:refs/tags/v0.1.292
git push -f origin 04fa983048598b4dc62405184eb74b3339851e8c:refs/tags/v0.1.303
git push -f origin a63e8096b6c0049e47aec589e8e43a3719c54c45:refs/tags/v0.1.306
git push -f origin 81a5bc066fe66da076471c86212f3b02f5ed989b:refs/tags/v0.1.308
git fetch origin --tags --force --prune --prune-tags
Write-Host "Pronto: pasta e versões no histórico novo."
