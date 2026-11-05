#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include <glm/glm.hpp>

namespace game {

// Áudio 3D sobre o miniaudio: sons presos a posições do mundo (atenuação pela distância, panorâmica pela direção em
// relação ao ouvinte, Doppler), em laço ou de uma vez, WAV/MP3/FLAC. O ouvinte é a câmera.
// offline = sem placa de som (modo sem janela, testes): a mixagem é calculada por mix() e pode ser gravada em WAV.
class Audio {
 public:
  explicit Audio(bool offline);
  ~Audio();
  Audio(const Audio&) = delete;
  Audio& operator=(const Audio&) = delete;
  bool ok() const;

  struct SoundDesc {
    float volume = 1.0f;
    bool loop = false;
    bool spatial = true;
    float minDistance = 1.0f;   // m: até aqui, volume cheio
    float maxDistance = 100.0f; // m: depois daqui, não cai mais
    float rolloff = 1.0f;       // atenuação inversa: ganho = min / (min + rolloff·(d − min))
  };
  // Carrega um som parado; ~0u se o arquivo não abrir.
  uint32_t create(const std::string& path, const SoundDesc& desc, glm::vec3 position);
  void play(uint32_t sound);
  void stop(uint32_t sound);
  void setPosition(uint32_t sound, glm::vec3 position, glm::vec3 velocity = glm::vec3(0.0f));
  // Toca um som uma vez numa posição (some sozinho ao terminar).
  void playOnce(const std::string& path, glm::vec3 position, float volume = 1.0f);

  void setListener(glm::vec3 position, glm::vec3 forward, glm::vec3 velocity = glm::vec3(0.0f));
  // Offline: mistura `seconds` de áudio (guardado para writeWav). Com placa de som, o miniaudio toca sozinho.
  void mix(float seconds);
  bool writeWav(const std::string& path) const;  // estéreo 48 kHz, 16 bits
  static constexpr uint32_t kSampleRate = 48000;

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

}  // namespace game
