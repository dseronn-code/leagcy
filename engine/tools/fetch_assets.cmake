# Baixa os modelos de exemplo oficiais da Khronos (glTF-Sample-Assets) para engine/assets/.
# Uso (de dentro de engine/):  cmake -P tools/fetch_assets.cmake
# Os arquivos não vão para o git (ver .gitignore).

set(BASE "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models")
set(MODELS Sponza DamagedHelmet Fox CesiumMan)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../assets" ABSOLUTE)

function(download url dest)
  if(EXISTS "${dest}")
    return()
  endif()
  message(STATUS "  baixando ${url}")
  file(DOWNLOAD "${url}" "${dest}" STATUS st TLS_VERIFY ON)
  list(GET st 0 code)
  if(NOT code EQUAL 0)
    file(REMOVE "${dest}")
    message(FATAL_ERROR "Falha ao baixar ${url}: ${st}")
  endif()
endfunction()

foreach(model ${MODELS})
  message(STATUS "${model}")
  set(dir "${ROOT}/${model}")
  set(gltf "${dir}/${model}.gltf")
  download("${BASE}/${model}/glTF/${model}.gltf" "${gltf}")
  # Baixa todos os arquivos referenciados por "uri" (buffers e texturas).
  file(READ "${gltf}" json)
  string(REGEX MATCHALL "\"uri\"[ \t\r\n]*:[ \t\r\n]*\"[^\"]+\"" uris "${json}")
  foreach(entry ${uris})
    string(REGEX REPLACE ".*\"([^\"]+)\"$" "\\1" uri "${entry}")
    if(uri MATCHES "^data:")
      continue()
    endif()
    download("${BASE}/${model}/glTF/${uri}" "${dir}/${uri}")
  endforeach()
endforeach()

# Bistro (Amazon Lumberyard / NVIDIA, licença MIT, ~2,4 GB): cena de teste de nível profissional.
# Versão de github.com/zeux/niagara_bistro (git comum, sem LFS; texturas BC7 em DDS).
set(BISTRO "${ROOT}/Bistro")
if(NOT EXISTS "${BISTRO}/bistro.gltf")
  message(STATUS "Bistro (2,4 GB, pode demorar)")
  find_package(Git REQUIRED)
  execute_process(COMMAND "${GIT_EXECUTABLE}" clone --depth 1 https://github.com/zeux/niagara_bistro.git "${BISTRO}"
                  RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "Falha ao clonar o Bistro")
  endif()
endif()
message(STATUS "Assets prontos em ${ROOT}")
