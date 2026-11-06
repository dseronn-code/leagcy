package com.treta.penalti;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.os.Bundle;
import android.view.View;
import android.view.WindowManager;

// A tela do jogo é a NativeActivity (o motor desenha direto na janela). Esta classe só deixa a tela cheia e acesa e
// mostra os erros do motor numa caixa de diálogo (no celular não há console para ler o log).
public class PenaltiActivity extends NativeActivity {
  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    hideSystemBars();
  }

  @Override
  public void onWindowFocusChanged(boolean hasFocus) {
    super.onWindowFocusChanged(hasFocus);
    if (hasFocus) hideSystemBars();
  }

  private void hideSystemBars() {
    getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_FULLSCREEN
        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
  }

  // Chamado pelo código nativo (android_main.cpp) quando o motor não consegue continuar.
  public void showError(final String message) {
    runOnUiThread(() -> new AlertDialog.Builder(this)
        .setTitle("Pênalti da Treta")
        .setMessage(message)
        .setCancelable(false)
        .setPositiveButton("Fechar", (d, w) -> finish())
        .show());
  }
}
