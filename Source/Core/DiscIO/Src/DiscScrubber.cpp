// Copyright (C) 2003-2009 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#include "stdafx.h"

#include "Filesystem.h"
#include "VolumeCreator.h"
#include "FileUtil.h"
#include "DiscScrubber.h"

namespace DiscIO
{

namespace DiscScrubber
{

#define SCRUBBER_VERSION 1

#define	CLUSTER_SIZE 0x8000

u8* m_Sector1;
u8* m_FreeTable;
u64 m_FileSize;

std::string m_Filename;
IVolume* m_Disc = NULL;

struct SPartitionHeader
{
	u8* Ticket[0x2a4];
	u32 TMDSize;
	u64 TMDOffset;
	u32 CertChainSize;
	u64 CertChainOffset;
	// H3Size is always 0x18000 
	u64 H3Offset;
	u64 DataOffset;
	u64 DataSize;
	// TMD would be here
	u64 DOLOffset;
	u64 DOLSize;
	u64 FSTOffset;
	u64 FSTSize;
	u32 ApploaderSize;
	u32 ApploaderTrailerSize;
};
struct SPartition
{
	u32 GroupNumber;
	u32 Number;
	u64 Offset;
	u32 Type;
	SPartitionHeader Header;
};
struct SPartitionGroup
{
	u32 numPartitions;
	u64 PartitionsOffset;
	std::vector<SPartition> PartitionsVec;
};
SPartitionGroup PartitionGroup[4];


void MarkAsUsed(u64 _Offset, u64 _Size);
void MarkAsUsedE(u64 _PartitionDataOffset, u64 _Offset, u64 _Size);
bool MarkAsScrubbed(const char* filename);
void ReadFromDisc(u64 _Offset, u64 _Length, u32& _Buffer);
void ReadFromDisc(u64 _Offset, u64 _Length, u64& _Buffer);
void ReadFromVolume(u64 _Offset, u64 _Length, u32& _Buffer);
void ReadFromVolume(u64 _Offset, u64 _Length, u64& _Buffer);
void ParseDisc();
void ParsePartitionData(SPartition& _rPartition);
u32 GetDOLSize(u64 _DOLOffset);


// Check for simplistic flag stored in an unencrypted pad space (offset 0x80)
u32 IsScrubbed(const char* filename)
{
	FILE* f = fopen(filename, "rb");

	if (!f)
		return false;

	u32 ScrubbedFlag = SCRUBBER_VERSION;
	fseek(f, 0x80, SEEK_SET);
	fread(&ScrubbedFlag, sizeof(ScrubbedFlag), 1, f);
	fclose(f);
	return ScrubbedFlag;
}

bool Scrub(const char* filename, CompressCB callback, void* arg)
{
	bool success = true;
	m_Filename = std::string(filename);

	u32 version = IsScrubbed(filename);
	if (version && version < SCRUBBER_VERSION)
	{
		if (!PanicYesNo("%s was scrubbed with an older version of DiscScrubber, would you like to re-scrub?", filename))
			return success;
	}
	else if (version)
	{
		callback("DiscScrubber: This disc is already scrubbed", 0, arg);
		NOTICE_LOG(DISCIO, "%s is already scrubbed, skipping...", filename);
		return success;
	}

	m_Disc = CreateVolumeFromFilename(filename);
	m_FileSize = m_Disc->GetSize();

	u64 numClusters = m_FileSize / CLUSTER_SIZE;

	// Warn if not DVD5 or DVD9 size
	if (numClusters != 0x23048 && numClusters != 0x46090)
		WARN_LOG(DISCIO, "%s is not a standard sized wii m_Disc! (%x blocks)", filename, numClusters);

	// Table of free blocks
	m_FreeTable = new u8[numClusters];
	std::fill(m_FreeTable, m_FreeTable + numClusters, 1);
	// Fill the dummy all 1 block
	m_Sector1 = new u8[CLUSTER_SIZE];
	std::fill(m_Sector1, m_Sector1 + CLUSTER_SIZE, 0xFF);

	// Fill out table of free blocks
	callback("DiscScrubber: Parsing...", 0, arg);
	ParseDisc();
	// Done with it; need it closed for the next part
	delete m_Disc;
	m_Disc = NULL;

	// Open file
	FILE* pFile = fopen(filename, "r+b");
	if (!pFile)
	{
		ERROR_LOG(DISCIO, "DiscScrubber failed to open %s", filename);
		success = false;
		return success;
	}

	// Modify file, obeying the table of free blocks
	NOTICE_LOG(DISCIO, "Removing garbage data...go get some coffee :)");
	for (u64 i = 0;	i < numClusters; i++)
	{
		u64 CurrentOffset = i * CLUSTER_SIZE;

		if (m_FreeTable[i])
		{
			DEBUG_LOG(DISCIO, "Freeing 0x%016llx", CurrentOffset);
			// Area is unused so fill with 1s
 			fseek(pFile, CurrentOffset, SEEK_SET);
 			success |= fwrite(m_Sector1, CLUSTER_SIZE, 1, pFile) == CLUSTER_SIZE;

			if (!success)
			{
				PanicAlert("DiscScrubber failure");
				break;
			}
		}
		else
		{
			DEBUG_LOG(DISCIO, "Used    0x%016llx", CurrentOffset);
		}

		// Update progress dialog
		if (i % (numClusters / 1000) == 0)
		{
			char temp[512];
			sprintf(temp, "DiscScrubber: %llu/%llu (%s)", i, numClusters, m_FreeTable[i] ? "Free" : "Used");
			callback(temp, (float)i / (float)numClusters, arg);
		}
	}
	NOTICE_LOG(DISCIO, "Done removing garbage data");

	fclose(pFile);

	delete m_Sector1;
	delete m_FreeTable;

	if (success)
		if (!MarkAsScrubbed(filename))
			ERROR_LOG(DISCIO, "Really weird - failed to mark scrubbed disk as scrubbed :s");

	return success;
}

void MarkAsUsed(u64 _Offset, u64 _Size)
{
	u64 CurrentOffset = _Offset;
	u64 EndOffset = CurrentOffset + _Size;

	DEBUG_LOG(DISCIO, "Marking 0x%016llx - 0x%016llx as used", _Offset, EndOffset);

	while ((CurrentOffset < EndOffset) && (CurrentOffset < m_FileSize))
	{
		m_FreeTable[CurrentOffset / CLUSTER_SIZE] = 0;
		CurrentOffset += CLUSTER_SIZE;
	}
}
// Compensate for 0x400(SHA-1) per 0x8000(cluster)
void MarkAsUsedE(u64 _PartitionDataOffset, u64 _Offset, u64 _Size)
{
	u64 Offset;
	u64 Size;

	Offset = _Offset / 0x7c00;
	Offset = Offset * CLUSTER_SIZE;
	Offset += _PartitionDataOffset;

	Size = _Size / 0x7c00;
	Size = (Size + 1) * CLUSTER_SIZE;

	// Add on the offset in the first block for the case where data straddles blocks
	Size += _Offset % 0x7c00;

	MarkAsUsed(Offset, Size);
}

bool MarkAsScrubbed(const char* filename)
{
	FILE* f = fopen(filename, "r+b");

	if (!f)
		return false;

	bool success;
	u8 ScrubbedFlag[1] = {SCRUBBER_VERSION};
	fseek(f, 0x80, SEEK_SET);
	success |= fwrite(ScrubbedFlag, 1, 1, f) == 1;
	fclose(f);
	return success;
}

// Helper functions for RAW reading the BE discs
void ReadFromDisc(u64 _Offset, u64 _Length, u32& _Buffer)
{
	m_Disc->RAWRead(_Offset, _Length, (u8*)&_Buffer);
	_Buffer = Common::swap32(_Buffer);
}
void ReadFromDisc(u64 _Offset, u64 _Length, u64& _Buffer)
{
	m_Disc->RAWRead(_Offset, _Length, (u8*)&_Buffer);
	_Buffer = Common::swap32((u32)_Buffer);
	_Buffer <<= 2;
}
// Helper functions for reading the BE volume
void ReadFromVolume(u64 _Offset, u64 _Length, u32& _Buffer)
{
	m_Disc->Read(_Offset, _Length, (u8*)&_Buffer);
	_Buffer = Common::swap32(_Buffer);
}
void ReadFromVolume(u64 _Offset, u64 _Length, u64& _Buffer)
{
	m_Disc->Read(_Offset, _Length, (u8*)&_Buffer);
	_Buffer = Common::swap32((u32)_Buffer);
	_Buffer <<= 2;
}

void ParseDisc()
{
	// Mark the header as used - it's mostly 0s anyways
	MarkAsUsed(0, 0x50000);

	for (int x = 0; x < 4; x++)
	{
		ReadFromDisc(0x40000 + (x * 8) + 0, 4, PartitionGroup[x].numPartitions);
		ReadFromDisc(0x40000 + (x * 8) + 4, 4, PartitionGroup[x].PartitionsOffset);

		// Read all partitions
		for (u32 i = 0; i < PartitionGroup[x].numPartitions; i++)
		{
			SPartition Partition;

			Partition.GroupNumber = x;
			Partition.Number = i;

			ReadFromDisc(PartitionGroup[x].PartitionsOffset + (i * 8) + 0, 4, Partition.Offset);
			ReadFromDisc(PartitionGroup[x].PartitionsOffset + (i * 8) + 4, 4, Partition.Type);

			ReadFromDisc(Partition.Offset + 0x2a4, 4, Partition.Header.TMDSize);
			ReadFromDisc(Partition.Offset + 0x2a8, 4, Partition.Header.TMDOffset);
			ReadFromDisc(Partition.Offset + 0x2ac, 4, Partition.Header.CertChainSize);
			ReadFromDisc(Partition.Offset + 0x2b0, 4, Partition.Header.CertChainOffset);
			ReadFromDisc(Partition.Offset + 0x2b4, 4, Partition.Header.H3Offset);
			ReadFromDisc(Partition.Offset + 0x2b8, 4, Partition.Header.DataOffset);
			ReadFromDisc(Partition.Offset + 0x2bc, 4, Partition.Header.DataSize);

			PartitionGroup[x].PartitionsVec.push_back(Partition);
		}

		for (size_t i = 0; i < PartitionGroup[x].PartitionsVec.size(); i++)
		{
			SPartition& rPartition			= PartitionGroup[x].PartitionsVec.at(i);
			const SPartitionHeader& rHeader	= PartitionGroup[x].PartitionsVec.at(i).Header;

			MarkAsUsed(rPartition.Offset, 0x2c0);

			MarkAsUsed(rPartition.Offset + rHeader.TMDOffset, rHeader.TMDSize);
			MarkAsUsed(rPartition.Offset + rHeader.CertChainOffset, rHeader.CertChainSize);
			MarkAsUsed(rPartition.Offset + rHeader.H3Offset, 0x18000);
			// This would mark the whole (encrypted) data area
			// we need to parse FST and other crap to find what's free within it!
			//MarkAsUsed(rPartition.Offset + rHeader.DataOffset, rHeader.DataSize);

			// Parse Data! This is where the big gain is
			ParsePartitionData(rPartition);
		}
	}
}

// Operations dealing with encrypted space are done here - the volume is swapped to allow this
void ParsePartitionData(SPartition& _rPartition)
{
	// Switch out the main volume temporarily
	IVolume *OldVolume = m_Disc;

	// Ready some stuff
	m_Disc = CreateVolumeFromFilename(m_Filename.c_str(), _rPartition.GroupNumber, _rPartition.Number);
	IFileSystem *FileSystem = CreateFileSystem(m_Disc);

	std::vector<const SFileInfo *> Files;
	size_t numFiles = FileSystem->GetFileList(Files);

	// Mark things as used which are not in the filesystem
	// Header, Header Information, Apploader
	ReadFromVolume(0x2440 + 0x14, 4, _rPartition.Header.ApploaderSize);
	ReadFromVolume(0x2440 + 0x18, 4, _rPartition.Header.ApploaderTrailerSize);
	MarkAsUsedE(_rPartition.Offset
		+ _rPartition.Header.DataOffset
		, 0
		, 0x2440
		+ _rPartition.Header.ApploaderSize
		+ _rPartition.Header.ApploaderTrailerSize);

	// DOL
	ReadFromVolume(0x420, 4, _rPartition.Header.DOLOffset);
	_rPartition.Header.DOLSize = GetDOLSize(_rPartition.Header.DOLOffset);
	MarkAsUsedE(_rPartition.Offset
		+ _rPartition.Header.DataOffset
		, _rPartition.Header.DOLOffset
		, _rPartition.Header.DOLSize);

	// FST
	ReadFromVolume(0x424, 4, _rPartition.Header.FSTOffset);
	ReadFromVolume(0x428, 4, _rPartition.Header.FSTSize);
	MarkAsUsedE(_rPartition.Offset
		+ _rPartition.Header.DataOffset
		, _rPartition.Header.FSTOffset
		, _rPartition.Header.FSTSize);

	// Go through the filesystem and mark entries as used
	for (size_t currentFile = 0; currentFile < numFiles; currentFile++)
	{
		DEBUG_LOG(DISCIO, "%s", (*Files.at(currentFile)).m_FullPath);
		// Just 1byte for directory? - it will end up reserving a cluster this way
		if ((*Files.at(currentFile)).m_NameOffset & 0x1000000)
			MarkAsUsedE(_rPartition.Offset
			+ _rPartition.Header.DataOffset
			, (*Files.at(currentFile)).m_Offset, 1);
		else
			MarkAsUsedE(_rPartition.Offset
			+ _rPartition.Header.DataOffset
			, (*Files.at(currentFile)).m_Offset, (*Files.at(currentFile)).m_FileSize);
	}

	delete FileSystem;

	// Swap back
	delete m_Disc;
	m_Disc = OldVolume;
}

u32 GetDOLSize(u64 _DOLOffset)
{
	u32 offset = 0, size = 0, max = 0;

	// Iterate through the 7 code segments
	for (u8 i = 0; i < 7; i++)
	{
		ReadFromVolume(_DOLOffset + 0x00 + i * 4, 4, offset);
		ReadFromVolume(_DOLOffset + 0x90 + i * 4, 4, size);
		if (offset + size > max)
			max = offset + size;
	}

	// Iterate through the 11 data segments
	for (u8 i = 0; i < 11; i++)
	{
		ReadFromVolume(_DOLOffset + 0x1c + i * 4, 4, offset);
		ReadFromVolume(_DOLOffset + 0xac + i * 4, 4, size);
		if (offset + size > max)
			max = offset + size;
	}

	return max;
}

} // namespace DiscScrubber

} // namespace DiscIO
