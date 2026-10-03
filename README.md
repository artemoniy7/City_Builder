# City Builder — DirectX 12 starter engine

Минимальный, но полноценный Win32/DirectX 12 каркас для Windows: приложение создаёт безрамочное окно, инициализирует устройство и swap chain DirectX 12, рисует вращающийся цветной куб и завершает работу по `Esc`.

## Структура

```text
assets/shaders/  HLSL-шейдеры
src/core/        цикл приложения и Win32-окно
src/graphics/    устройство, swap chain, команда рендера и геометрия
src/math/        компактные матрицы для сцены
```

## Сборка

Нужны Windows 10/11, Visual Studio 2022 с компонентом **Desktop development with C++**, Windows SDK и CMake 3.21+.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
.\build\Debug\CityBuilder.exe
```

При запуске шейдер копируется рядом с `.exe` в папку `shaders`. `Esc` корректно дожидается GPU и закрывает приложение.

