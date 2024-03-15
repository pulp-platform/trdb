/*
 * trdb - Debugger Software for the PULP platform
 *
 * Copyright (C) 2024 Robert Balas
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file trdb_private.h
 * @author Robert Balas (balasr@student.ethz.ch)
 * @brief Software model for the hardware trace debugger.
 */

#include <stdint.h>

uint32_t branch_map_len(uint32_t branches);
