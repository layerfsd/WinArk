#include "pch.h"
#include "FileManager.h"
#include "ObjectAttributes.h"
#include "kstring.h"
#include "Memory.h"

NTSTATUS FileManager::Open(PUNICODE_STRING fileName, FileAccessMask accessMask /* = FileAccessMask::All */) {
	NTSTATUS status;
	IO_STATUS_BLOCK ioStatus;
	
	ObjectAttributes attr(fileName, ObjectAttributesFlags::Caseinsensive | ObjectAttributesFlags::KernelHandle);
	status = ZwCreateFile(&_handle, static_cast<ULONG>(accessMask), &attr, &ioStatus, nullptr,
		FILE_ATTRIBUTE_NORMAL,
		FILE_SHARE_READ,
		FILE_OPEN_IF, // �򿪻򴴽��ļ�
		FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT| // �ļ���ͬ���Ĵ�
		FILE_RANDOM_ACCESS,
		nullptr, 0);
	return status;
}

NTSTATUS FileManager::OpenFileForRead(PCWSTR path) {
	UNICODE_STRING name;
	RtlInitUnicodeString(&name, path);

	ObjectAttributes attr(&name, ObjectAttributesFlags::Caseinsensive | ObjectAttributesFlags::KernelHandle);
	IO_STATUS_BLOCK ioStatus;
	return ZwOpenFile(&_handle, FILE_GENERIC_READ, &attr, &ioStatus, FILE_SHARE_READ, 0);
}

NTSTATUS FileManager::Close() {
	if (_handle)
		return ZwClose(_handle);
}

FileManager::~FileManager() {
	Close();
}

NTSTATUS FileManager::ReadFile(PVOID buffer,ULONG size,PIO_STATUS_BLOCK ioStatus,PLARGE_INTEGER offset) {
	NTSTATUS status;

	status = ZwReadFile(_handle, 
		nullptr, // ͬ����
		nullptr, // 
		nullptr, //
		ioStatus, buffer, size, 
		offset, // specifies the starting byte offset in the file where the read operation will begin
		nullptr);
	return status;
}


NTSTATUS FileManager::WriteFile(PVOID buffer, ULONG size, PIO_STATUS_BLOCK ioStatus, PLARGE_INTEGER offset) {
	NTSTATUS status;

	status = ZwWriteFile(_handle,
		nullptr,// ͬ��д
		nullptr,
		nullptr,
		ioStatus,buffer,size,offset,nullptr);

	return status;
}

NTSTATUS FileManager::GetFileSize(PLARGE_INTEGER fileSize) {
	FILE_STANDARD_INFORMATION info = { 0 };
	IO_STATUS_BLOCK ioStatus;
	NTSTATUS status = ZwQueryInformationFile(_handle, &ioStatus, &info, sizeof(info), FileStandardInformation);
	if (NT_SUCCESS(status)) {
		if (!fileSize) {
			return STATUS_INVALID_PARAMETER;
		}
		*fileSize = info.EndOfFile;
		return status;
	}
	return status;
}

/*
Note   Do not specify 
FILE_READ_DATA, FILE_WRITE_DATA, FILE_APPEND_DATA, or FILE_EXECUTE 
when you create or open a directory.
*/
NTSTATUS FileManager::DeleteFile(PUNICODE_STRING fileName) {
	NTSTATUS status;
	IO_STATUS_BLOCK ioStatus;

	ObjectAttributes attr(fileName, ObjectAttributesFlags::Caseinsensive | ObjectAttributesFlags::KernelHandle);
	status = ZwDeleteFile(&attr);
	return status;
}

NTSTATUS FileManager::ConvertDosNameToNtName(_In_ PCWSTR dosName, _Out_ PUNICODE_STRING ntName) {
	ntName->Buffer = nullptr;	// in case of failure
	auto dosNameLen = ::wcslen(dosName);

	if (dosNameLen < 3)
		return STATUS_BUFFER_TOO_SMALL;

	// make sure we have a driver letter
	if (dosName[2] != L'\\' || dosName[1] != L':')
		return STATUS_INVALID_PARAMETER;

	kstring<'rgmf'> symLink(L"\\??\\");
	symLink.Append(dosName, 2);		// driver letter and colon

	UNICODE_STRING symLinkFull;
	symLink.GetUnicodeString(&symLinkFull);

	OBJECT_ATTRIBUTES symLinkAttr;
	InitializeObjectAttributes(&symLinkAttr, &symLinkFull,
		OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, nullptr, nullptr);

	HANDLE hSymLink = nullptr;
	auto status = STATUS_SUCCESS;

	do
	{
		// open symbolic link
		status = ZwOpenSymbolicLinkObject(&hSymLink, GENERIC_READ, &symLinkAttr);
		if (!NT_SUCCESS(status))
			break;
		USHORT maxLen = 1024;	// arbitray
		ntName->Buffer = (WCHAR*)ExAllocatePool(PagedPool, maxLen);
		if (!ntName->Buffer) {
			status = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}
		ntName->MaximumLength = maxLen;

		// read target of symbolic link
		status = ZwQuerySymbolicLinkObject(hSymLink, ntName, nullptr);
		if (!NT_SUCCESS(status))
			break;
	} while (false);

	if (!NT_SUCCESS(status)) {
		if (ntName->Buffer) {
			ExFreePool(ntName->Buffer);
			ntName->Buffer = nullptr;
		}
	}
	else {
		RtlAppendUnicodeToString(ntName, dosName + 2);	// directory part
	}

	if (hSymLink)
		ZwClose(hSymLink);

	return status;
}

NTSTATUS FileManager::GetRootName(_In_ PCWSTR dosName, _Out_ PUNICODE_STRING rootName) {
	UNICODE_STRING name;
	name.Buffer = nullptr;
	
	auto dosNameLen = ::wcslen(dosName);

	if (dosNameLen < 3)
		return STATUS_BUFFER_TOO_SMALL;

	// make sure we have a driver letter
	if (dosName[2] != L'\\' || dosName[1] != L':')
		return STATUS_INVALID_PARAMETER;

	kstring<'rgmf'> symLink(L"\\??\\");
	symLink.Append(dosName, 3);		// driver letter,colon,backslash
	symLink.GetUnicodeString(&name);

	rootName->Buffer = (WCHAR*)ExAllocatePool(PagedPool, name.Length);
	RtlCopyUnicodeString(rootName, &name);
	rootName->MaximumLength = name.Length;

	return STATUS_SUCCESS;
}

VOID FileManager::FreeMdl(_In_ PMDL mdl){
	PMDL pCur = mdl, pNext;

	while (pCur != nullptr) {
		pNext = pCur->Next;
		if (pCur->MdlFlags & MDL_PAGES_LOCKED) {
			MmUnlockPages(pCur);
		}
		IoFreeMdl(pCur);
		pCur = pNext;
	}
}

NTSTATUS IoCompletionRoutine(
	PDEVICE_OBJECT DeviceObject,
	PIRP Irp, 
	PVOID Context){

	PKEVENT UserEvent = Irp->UserEvent;

	ASSERT(UserEvent != nullptr);
	// �������״̬
	RtlCopyMemory(Irp->UserIosb, &Irp->IoStatus, sizeof(IO_STATUS_BLOCK));
	
	// ��鲢�ͷ�MDL
	if (Irp->MdlAddress != nullptr) {
		FileManager::FreeMdl(Irp->MdlAddress);
		Irp->MdlAddress = nullptr;
	}

	// �ͷ�Irp
	IoFreeIrp(Irp);

	KeSetEvent(UserEvent, IO_NO_INCREMENT, FALSE);

	return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS FileManager::IrpCreateFile(
	_Out_ PFILE_OBJECT* pFileObject,
	_Out_ PDEVICE_OBJECT* pDeviceObject,
	_In_ ACCESS_MASK DesiredAccess,
	_In_ PCWSTR DosName,
	_Out_ PIO_STATUS_BLOCK IoStatusBlock,
	_In_opt_ PLARGE_INTEGER AllocationSize,
	_In_ ULONG FileAttributes,
	_In_ ULONG ShareAccess,
	_In_ ULONG CreateDisposition,
	_In_ ULONG CreateOptions,
	_In_reads_bytes_opt_(EaLength) PVOID EaBuffer,
	_In_ ULONG EaLength
) {
	PAUX_ACCESS_DATA AuxData = nullptr;
	HANDLE hRoot = nullptr;
	PFILE_OBJECT RootObject = nullptr, FileObject = nullptr;
	PDEVICE_OBJECT DeviceObject = nullptr, RealDeviceObject = nullptr;
	PIRP Irp;
	PIO_STACK_LOCATION stack;
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	ACCESS_STATE AccessState;
	KEVENT notifyEvent;
	IO_SECURITY_CONTEXT IoSecurityContext;
	UNICODE_STRING rootName, fileName;
	RtlInitUnicodeString(&fileName, &DosName[2]);

	do
	{
		// �򿪸�Ŀ¼
		IO_STATUS_BLOCK ioStatus;
		status = GetRootName(DosName, &rootName);
		if (!NT_SUCCESS(status)) {
			break;
		}
		ObjectAttributes attr(&rootName, ObjectAttributesFlags::Caseinsensive | ObjectAttributesFlags::KernelHandle);
		status = IoCreateFile(&hRoot, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &attr, &ioStatus, nullptr,
			FILE_ATTRIBUTE_NORMAL,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			FILE_OPEN,
			FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
			nullptr, 0, CreateFileTypeNone, nullptr, IO_NO_PARAMETER_CHECKING);
		*IoStatusBlock = ioStatus;
		if (!NT_SUCCESS(status)) {
			break;
		}

		// �õ���Ŀ¼�ļ�����
		status = ObReferenceObjectByHandle(hRoot,
			FILE_READ_ATTRIBUTES,
			*IoFileObjectType,
			KernelMode,
			(PVOID*)&RootObject,
			nullptr);
		if (!NT_SUCCESS(status)) {
			break;
		}

		if (RootObject->Vpb == nullptr ||
			RootObject->Vpb->DeviceObject == nullptr ||
			RootObject->Vpb->RealDevice == nullptr) {
			status = STATUS_INVALID_PARAMETER;
			break;
		}

		// �õ��ļ�ϵͳ���豸
		DeviceObject = RootObject->Vpb->DeviceObject;
		// ����������ɵľ��豸�������豸��
		RealDeviceObject = RootObject->Vpb->RealDevice;
		RootObject = nullptr;
		ZwClose(hRoot);
		hRoot = nullptr;

		// �����ļ�����
		attr = ObjectAttributes(nullptr, ObjectAttributesFlags::Caseinsensive);
		status = ObCreateObject(KernelMode, *IoFileObjectType, &attr, KernelMode, nullptr,
			sizeof(FILE_OBJECT), 0, 0, (PVOID*)&FileObject);
		if (!NT_SUCCESS(status)) {
			break;
		}
		RtlZeroMemory(FileObject, sizeof(FILE_OBJECT));
		FileObject->Type = IO_TYPE_FILE;
		FileObject->Size = sizeof(FILE_OBJECT);
		FileObject->DeviceObject = RealDeviceObject;
		FileObject->Flags = FO_SYNCHRONOUS_IO;
		FileObject->FileName.Buffer = fileName.Buffer;
		FileObject->FileName.Length = fileName.Length;
		FileObject->FileName.MaximumLength = fileName.MaximumLength;
		KeInitializeEvent(&FileObject->Lock,
			SynchronizationEvent, FALSE);
		// ����AUX_ACCESS_DATA������
		AuxData = new (NonPagedPool) AUX_ACCESS_DATA;
		if (AuxData == nullptr) {
			status = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}

		status = SeCreateAccessState(&AccessState,
			AuxData, DesiredAccess,
			IoGetFileObjectGenericMapping());
		if (!NT_SUCCESS(status)) {
			break;
		}
		
		// ����IRP
		Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
		if (Irp == nullptr) {
			status = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}

		// ��дIrp
		Irp->MdlAddress = nullptr;
		Irp->UserBuffer = nullptr;
		Irp->AssociatedIrp.SystemBuffer = EaBuffer;
		Irp->Flags = IRP_CREATE_OPERATION | IRP_SYNCHRONOUS_API;
		Irp->RequestorMode = KernelMode;
		Irp->PendingReturned = FALSE;
		Irp->Cancel = FALSE;
		Irp->CancelRoutine = nullptr;
		KeInitializeEvent(&notifyEvent, NotificationEvent, FALSE);
		Irp->UserEvent = &notifyEvent;
		Irp->UserIosb = &ioStatus;
		Irp->Overlay.AllocationSize.QuadPart = AllocationSize != nullptr ? AllocationSize->QuadPart : 0;
		Irp->Tail.Overlay.Thread = PsGetCurrentThread();
		Irp->Tail.Overlay.OriginalFileObject = FileObject;
		Irp->Tail.Overlay.AuxiliaryBuffer = nullptr;

		// �õ��²�ջ�ռ�
		stack = IoGetNextIrpStackLocation(Irp);
		ASSERT(stack != nullptr);

		stack->MajorFunction = IRP_MJ_CREATE;
		IoSecurityContext.SecurityQos = nullptr;
		IoSecurityContext.AccessState = &AccessState;
		IoSecurityContext.DesiredAccess = DesiredAccess;
		IoSecurityContext.FullCreateOptions = 0;
		stack->Parameters.Create.SecurityContext = &IoSecurityContext;
		stack->Parameters.Create.Options = (CreateDisposition << 24) | CreateOptions;
		stack->Parameters.Create.FileAttributes = (USHORT)FileAttributes;
		stack->Parameters.Create.ShareAccess = (USHORT)ShareAccess;
		stack->Parameters.Create.EaLength = EaLength;
		stack->FileObject = FileObject;
		stack->DeviceObject = DeviceObject;

		// �����������
		IoSetCompletionRoutine(Irp, IoCompletionRoutine, nullptr, TRUE, TRUE, TRUE);

		// �·�Irp����
		status = IoCallDriver(DeviceObject, Irp);

		// �ȴ����
		KeWaitForSingleObject(&notifyEvent, Executive, KernelMode, FALSE, nullptr);
		
		// �õ����״̬
		*IoStatusBlock = ioStatus;

		status = IoStatusBlock->Status;
	} while (false);

	if (NT_SUCCESS(status)) {
		//�ɹ���Ĵ���
		ASSERT(FileObject != NULL && FileObject->DeviceObject != NULL);
		InterlockedIncrement(&FileObject->DeviceObject->ReferenceCount);
		if (FileObject->Vpb != NULL) {
			InterlockedIncrement((PLONG)&FileObject->Vpb->ReferenceCount);
		}
		*pFileObject = FileObject;
		*pDeviceObject = DeviceObject;
	}
	else {
		//ʧ�ܺ�Ĵ���
		if (FileObject != NULL) {
			FileObject->DeviceObject = NULL;
			ObDereferenceObject(FileObject);
		}
		IoStatusBlock->Status = status;
	}

	//��鲢�ͷ������Դ
	if (AuxData != NULL) {
		delete AuxData;
	}
	if (RootObject != NULL) {
		ObDereferenceObject(RootObject);
	}
	if (hRoot != NULL) {
		ZwClose(hRoot);
	}
	if (rootName.Buffer != NULL) {
		RtlFreeUnicodeString(&rootName);
	}

	return status;
}

//��ȡĿ¼�µ��ļ�����Ŀ¼(MajorFunction: IRP_MJ_DIRECTORY_CONTROL, MinorFunction: IRP_MN_QUERY_DIRECTORY)
NTSTATUS FileManager::IrpQueryDirectoryFile(
	_In_ PDEVICE_OBJECT DeviceObject,
	_In_ PFILE_OBJECT FileObject,
	_Out_ PIO_STATUS_BLOCK IoStatusBlock,
	_Out_writes_bytes_(Length) PVOID FileInformation,
	_In_ ULONG Length,
	_In_ FILE_INFORMATION_CLASS FileInformationClass,
	_In_ BOOLEAN ReturnSingleEntry,
	_In_opt_ PUNICODE_STRING FileName,
	_In_ BOOLEAN RestartScan
)
{
	PIRP Irp;
	PIO_STACK_LOCATION stack;
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	KEVENT notifyEvent;
	IO_STATUS_BLOCK ioStatus;

	//����IRP
	Irp = IoAllocateIrp(DeviceObject->StackSize, FALSE);
	RtlZeroMemory(IoStatusBlock, sizeof(IO_STATUS_BLOCK));
	if (Irp == NULL) {
		IoStatusBlock->Status = STATUS_INSUFFICIENT_RESOURCES;
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	//��дIRP
	Irp->MdlAddress = nullptr;
	Irp->AssociatedIrp.SystemBuffer = nullptr;
	Irp->UserBuffer = FileInformation;
	Irp->Flags = IRP_SYNCHRONOUS_API;
	Irp->RequestorMode = KernelMode;
	Irp->UserIosb = &ioStatus;
	KeInitializeEvent(&notifyEvent,
		NotificationEvent,
		FALSE);
	Irp->UserEvent = &notifyEvent;
	Irp->Tail.Overlay.Thread = PsGetCurrentThread();
	Irp->Tail.Overlay.OriginalFileObject = FileObject;

	//�õ��²�ջ�ռ�
	stack = IoGetNextIrpStackLocation(Irp);
	ASSERT(stack != NULL);

	//��д�²�ջ�ռ�
	stack->MajorFunction = IRP_MJ_DIRECTORY_CONTROL;
	stack->MinorFunction = IRP_MN_QUERY_DIRECTORY;
	stack->Flags = 0;
	if (ReturnSingleEntry)
		stack->Flags |= SL_RETURN_SINGLE_ENTRY;
	if (RestartScan)
		stack->Flags |= SL_RESTART_SCAN;
	stack->Parameters.QueryDirectory.Length = Length;
	stack->Parameters.QueryDirectory.FileName = FileName;
	stack->Parameters.QueryDirectory.FileInformationClass = FileInformationClass;
	stack->DeviceObject = DeviceObject;
	stack->FileObject = FileObject;

	//�����������: IoSetCompletionRoutine(pIrp, FileOperationCompletion, NULL, TRUE, TRUE, TRUE);
	stack->CompletionRoutine = IoCompletionRoutine;
	stack->Context = NULL;
	stack->Control = SL_INVOKE_ON_SUCCESS | SL_INVOKE_ON_ERROR | SL_INVOKE_ON_CANCEL;

	//�·�IO����
	status = IoCallDriver(DeviceObject, Irp);

	//�Ⱥ��������
	KeWaitForSingleObject(&notifyEvent,
		Executive,
		KernelMode,
		FALSE,
		NULL);

	//�õ����״̬
	*IoStatusBlock = ioStatus;
	status = IoStatusBlock->Status;

	return status;
}

/*
ZwQueryInformationFile(
	_In_ HANDLE FileHandle,
	_Out_ PIO_STATUS_BLOCK IoStatusBlock,
	_Out_writes_bytes_(Length) PVOID FileInformation,
	_In_ ULONG Length,
	_In_ FILE_INFORMATION_CLASS FileInformationClass
	);
*/
//�ļ���ѯ(IRP_MJ_QUERY_INFORMATION)
NTSTATUS FileManager::IrpQueryInformationFile(
	_In_ PDEVICE_OBJECT DeviceObject,
	_In_ PFILE_OBJECT FileObject,
	_Out_ PIO_STATUS_BLOCK IoStatusBlock,
	_Out_writes_bytes_(Length) PVOID FileInformation,
	_In_ ULONG Length,
	_In_ FILE_INFORMATION_CLASS FileInformationClass
){
	PIRP Irp;
	PIO_STACK_LOCATION stack;
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	IO_STATUS_BLOCK ioStatus;
	KEVENT notifyEvent;

	//����IRP
	Irp = IoAllocateIrp(DeviceObject->StackSize,FALSE);
	RtlZeroMemory(IoStatusBlock,sizeof(IO_STATUS_BLOCK));
	if (Irp == NULL) {
		IoStatusBlock->Status = STATUS_INSUFFICIENT_RESOURCES;
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	//��дIRP
	Irp->MdlAddress = NULL;
	Irp->AssociatedIrp.SystemBuffer = FileInformation;
	Irp->UserBuffer = NULL;
	Irp->Flags = IRP_SYNCHRONOUS_API;
	Irp->RequestorMode = KernelMode;
	Irp->UserIosb = &ioStatus;
	KeInitializeEvent(&notifyEvent,
		NotificationEvent,
		FALSE);
	Irp->UserEvent = &notifyEvent;
	Irp->Tail.Overlay.Thread = PsGetCurrentThread();
	Irp->Tail.Overlay.OriginalFileObject = FileObject;

	//��ȡ�²�ջ�ռ�
	stack = IoGetNextIrpStackLocation(Irp);
	ASSERT(stack != NULL);

	//��дջ�ռ�
	stack->MajorFunction = IRP_MJ_QUERY_INFORMATION;
	stack->Parameters.QueryFile.FileInformationClass = FileInformationClass;
	stack->Parameters.QueryFile.Length = Length;
	stack->FileObject = FileObject;
	stack->DeviceObject = DeviceObject;

	//�����������: IoSetCompletionRoutine(pIrp,FileOperationCompletion,NULL,TRUE,TRUE,TRUE);
	stack->CompletionRoutine = IoCompletionRoutine;
	stack->Context = NULL;
	stack->Control = SL_INVOKE_ON_SUCCESS | SL_INVOKE_ON_ERROR | SL_INVOKE_ON_CANCEL;

	//�·�IRP����
	status = IoCallDriver(DeviceObject,Irp);

	//�Ⱥ��������
	KeWaitForSingleObject(&notifyEvent,
		Executive,
		KernelMode,
		FALSE,
		NULL);

	//�õ����״̬
	*IoStatusBlock = ioStatus;
	status = IoStatusBlock->Status;

	return status;
}

//�ļ�����(IRP_MJ_SET_INFORMATION��
NTSTATUS FileManager::IrpSetInformationFile(
	_In_ PDEVICE_OBJECT DeviceObject,
	_In_ PFILE_OBJECT FileObject,
	_In_opt_ PFILE_OBJECT TargetFileObject,
	_Out_ PIO_STATUS_BLOCK IoStatusBlock,
	_In_reads_bytes_(Length) PVOID FileInformation,
	_In_ ULONG Length,
	_In_ FILE_INFORMATION_CLASS FileInformationClass
)
{
	PIRP Irp = NULL;
	PIO_STACK_LOCATION stack;
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	IO_STATUS_BLOCK ioStatus;
	KEVENT notifyEvent;

	//����IRP
	Irp = IoAllocateIrp(DeviceObject->StackSize,FALSE);
	RtlZeroMemory(IoStatusBlock,sizeof(IO_STATUS_BLOCK));
	if (Irp == NULL) {
		IoStatusBlock->Status = STATUS_INSUFFICIENT_RESOURCES;
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	//��дIRP
	Irp->MdlAddress = NULL;
	Irp->UserBuffer = NULL;
	Irp->AssociatedIrp.SystemBuffer = FileInformation;
	Irp->Flags = IRP_SYNCHRONOUS_API;
	Irp->RequestorMode = KernelMode;
	Irp->UserIosb = &ioStatus;
	KeInitializeEvent(&notifyEvent,NotificationEvent,FALSE);
	Irp->UserEvent = &notifyEvent;
	Irp->Tail.Overlay.Thread = PsGetCurrentThread();
	Irp->Tail.Overlay.OriginalFileObject = FileObject;

	//�õ��²�ջ�ռ�
	stack = IoGetNextIrpStackLocation(Irp);
	ASSERT(stack != NULL);

	//��дջ�ռ�
	stack->MajorFunction = IRP_MJ_SET_INFORMATION;
	stack->Parameters.SetFile.Length = Length;
	stack->Parameters.SetFile.FileInformationClass = FileInformationClass;
	stack->Parameters.SetFile.FileObject = TargetFileObject;
	//�����ļ�������,����Ӳ������Ҫ����: ReplaceIfExists
	switch (FileInformationClass) {
	case FileRenameInformation:
		stack->Parameters.SetFile.ReplaceIfExists = ((PFILE_RENAME_INFORMATION)FileInformation)->ReplaceIfExists;
		break;
	case FileLinkInformation:
		stack->Parameters.SetFile.ReplaceIfExists = ((PFILE_LINK_INFORMATION)FileInformation)->ReplaceIfExists;
		break;
	}
	stack->FileObject = FileObject;
	stack->DeviceObject = DeviceObject;

	//�����������: IoSetCompletionRoutine(pIrp,FileOperationCompletion,NULL,TRUE,TRUE,TRUE);
	stack->CompletionRoutine = IoCompletionRoutine;
	stack->Context = NULL;
	stack->Control = SL_INVOKE_ON_SUCCESS | SL_INVOKE_ON_ERROR | SL_INVOKE_ON_CANCEL;

	//�·�IRP����
	status = IoCallDriver(DeviceObject,Irp);

	//�Ⱥ�IRP���
	KeWaitForSingleObject((PVOID)&notifyEvent,
		Executive,
		KernelMode,
		FALSE,
		NULL);

	//�������״̬
	*IoStatusBlock = ioStatus;
	status = IoStatusBlock->Status;

	return status;
}

//�Լ�����IRP����ر��ļ��ĵ�1��(IRP_MJ_CLEANUP),��ʾ�ļ���������Ϊ0
NTSTATUS FileManager::IrpCleanupFile(
	_In_ PDEVICE_OBJECT DeviceObject,
	_In_ PFILE_OBJECT FileObject
)
{
	PIRP Irp;
	PIO_STACK_LOCATION stack;
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	KEVENT notifyEvent;
	IO_STATUS_BLOCK ioStatus;

	//���������
	if (FileObject->Vpb == NULL || FileObject->Vpb->DeviceObject == NULL) {
		return STATUS_INVALID_PARAMETER;
	}

	//
	// IRP_MJ_CLEANUP
	//
	//����IRP
	Irp = IoAllocateIrp(DeviceObject->StackSize,FALSE);
	if (Irp == NULL) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	//��дIRP
	Irp->MdlAddress = nullptr;
	Irp->AssociatedIrp.SystemBuffer = nullptr;
	Irp->UserBuffer = nullptr;
	Irp->Flags = IRP_CLOSE_OPERATION | IRP_SYNCHRONOUS_API;
	Irp->RequestorMode = KernelMode;
	Irp->UserIosb = &ioStatus;
	KeInitializeEvent(&notifyEvent,
		NotificationEvent,
		FALSE);
	Irp->UserEvent = &notifyEvent;
	Irp->Tail.Overlay.Thread = PsGetCurrentThread();
	Irp->Tail.Overlay.OriginalFileObject = FileObject;

	//�õ��²�ջ�ռ�
	stack = IoGetNextIrpStackLocation(Irp);
	ASSERT(stack != NULL);

	//��д�²�ջ�ռ�
	stack->MajorFunction = IRP_MJ_CLEANUP;
	stack->FileObject = FileObject;
	stack->DeviceObject = DeviceObject;

	//�����������: IoSetCompletionRoutine(pIrp,FileOperationCompletion,NULL,TRUE,TRUE,TRUE);
	stack->CompletionRoutine = IoCompletionRoutine;
	stack->Context = nullptr;
	stack->Control = SL_INVOKE_ON_SUCCESS | SL_INVOKE_ON_ERROR | SL_INVOKE_ON_CANCEL;

	//�·�����
	status = IoCallDriver(DeviceObject,Irp);

	//�ȴ��������
	KeWaitForSingleObject(&notifyEvent,
		Executive,
		KernelMode,
		FALSE,
		NULL);

	//�õ����״̬
	status = ioStatus.Status;

	return status;
}

//�Լ�����IRP����ر��ļ��ĵ�2��(IRP_MJ_CLOSE),��ʾ���ü�����Ϊ0
NTSTATUS FileManager::IrpCloseFile(
	_In_ PDEVICE_OBJECT DeviceObject,
	_In_ PFILE_OBJECT FileObject
){
	PIRP Irp;
	PIO_STACK_LOCATION stack;
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	KEVENT notifyEvent;
	IO_STATUS_BLOCK ioStatus;

	//��鲢�����ļ���ȡ����־
	if (FileObject->Vpb != NULL && !(FileObject->Flags & FO_DIRECT_DEVICE_OPEN)) {
		InterlockedDecrement(reinterpret_cast<LONG*>(&FileObject->Vpb->ReferenceCount));
		FileObject->Flags |= FO_FILE_OPEN_CANCELLED;
	}

	//����IRP
	Irp = IoAllocateIrp(DeviceObject->StackSize,
		FALSE);
	if (Irp == NULL) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	//��дIRP
	Irp->MdlAddress = nullptr;
	Irp->AssociatedIrp.SystemBuffer = nullptr;
	Irp->UserBuffer = nullptr;
	Irp->Flags = IRP_CLOSE_OPERATION | IRP_SYNCHRONOUS_API;
	Irp->RequestorMode = KernelMode;
	Irp->UserIosb = &ioStatus;
	KeInitializeEvent(&notifyEvent,
		NotificationEvent,
		FALSE);
	Irp->UserEvent = &notifyEvent;
	Irp->Tail.Overlay.Thread = PsGetCurrentThread();
	Irp->Tail.Overlay.OriginalFileObject = FileObject;

	//�õ�IRP�²�ջ�ռ�
	stack = IoGetNextIrpStackLocation(Irp);
	ASSERT(stack != NULL);

	//��дIRP�²�ջ�ռ�
	stack->MajorFunction = IRP_MJ_CLOSE;
	stack->FileObject = FileObject;
	stack->DeviceObject = DeviceObject;

	//�����������: IoSetCompletionRoutine(pIrp,FileOperationCompletion,NULL,TRUE,TRUE,TRUE);
	stack->CompletionRoutine = IoCompletionRoutine;
	stack->Context = NULL;
	stack->Control = SL_INVOKE_ON_SUCCESS | SL_INVOKE_ON_ERROR | SL_INVOKE_ON_CANCEL;

	//�·�IRP����
	status = IoCallDriver(DeviceObject,Irp);

	//�ȴ��������
	KeWaitForSingleObject(&notifyEvent,Executive,
		KernelMode,
		FALSE,
		NULL);

	//�õ����״̬
	status = ioStatus.Status;

	return status;
}

//�Լ�����IRP����ɾ���ļ�
NTSTATUS FileManager::IrpDeleteFile(
	IN PDEVICE_OBJECT DeviceObject, //���ָ��ΪpFileObject->Vpb->DeviceObject,���ƹ��ļ�ϵͳ��������
	IN PFILE_OBJECT FileObject
){
	PFILE_BASIC_INFORMATION fileBasicInfo = NULL;
	PFILE_DISPOSITION_INFORMATION fileDispInfo = NULL;
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	IO_STATUS_BLOCK ioStatus;
	SECTION_OBJECT_POINTERS secObjPointers;

	PAGED_CODE();

	ASSERT(DeviceObject != NULL && FileObject != NULL);
	do {
		//�������Ի�����
		fileBasicInfo = new (PagedPool) FILE_BASIC_INFORMATION;
		if (fileBasicInfo == nullptr) {
			status = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}

		//�·�IRP_MJ_QUERY_INFORMATION�����ѯ�ļ�����
		status = IrpQueryInformationFile(DeviceObject,FileObject,
			&ioStatus,
			(PVOID)fileBasicInfo,
			sizeof(FILE_BASIC_INFORMATION),
			FileBasicInformation);
		if (!NT_SUCCESS(status)) {
			break;
		}
		
		//��鲢ȥ��ֻ������
		if (fileBasicInfo->FileAttributes & FILE_ATTRIBUTE_READONLY) {
			fileBasicInfo->FileAttributes &= ~FILE_ATTRIBUTE_READONLY;

			//�·�IRP_MJ_SET_INFORMATION����ȥ���ļ�ֻ������
			status = IrpSetInformationFile(DeviceObject,
				FileObject,
				NULL,
				&ioStatus,
				(PVOID)fileBasicInfo,
				sizeof(FILE_BASIC_INFORMATION),
				FileBasicInformation);
			if (!NT_SUCCESS(status)) {
				break;
			}
		}
		

		delete fileBasicInfo;
		fileBasicInfo = nullptr;

		

		//����ɾ������������
		fileDispInfo = new (PagedPool) FILE_DISPOSITION_INFORMATION;
		
		if (fileDispInfo == NULL) {
			status = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}
		fileDispInfo->DeleteFile = TRUE; //�ر�ʱɾ��

		//��鲢�����ļ������SectionObjectPointer��ָ������,Ȼ������ÿ�
		if (FileObject->SectionObjectPointer != NULL) {
			secObjPointers.DataSectionObject = FileObject->SectionObjectPointer->DataSectionObject;
			secObjPointers.SharedCacheMap = FileObject->SectionObjectPointer->SharedCacheMap;
			secObjPointers.ImageSectionObject = FileObject->SectionObjectPointer->ImageSectionObject;

			FileObject->SectionObjectPointer->DataSectionObject = nullptr;
			FileObject->SectionObjectPointer->SharedCacheMap = nullptr;
			FileObject->SectionObjectPointer->ImageSectionObject = nullptr;
		}

		//�·�IRP_MJ_SET_INFORMATION����,���ùر�ʱɾ��
		status = IrpSetInformationFile(DeviceObject,
			FileObject,
			NULL,
			&ioStatus,
			(PVOID)fileDispInfo,
			sizeof(FILE_DISPOSITION_INFORMATION),
			FileDispositionInformation);

		//��鲢�ָ��ļ������SectionObjectPointer��ָ������
		if (FileObject->SectionObjectPointer != NULL) {
			FileObject->SectionObjectPointer->DataSectionObject = secObjPointers.DataSectionObject;
			FileObject->SectionObjectPointer->SharedCacheMap = secObjPointers.SharedCacheMap;
			FileObject->SectionObjectPointer->ImageSectionObject = secObjPointers.ImageSectionObject;
		}
	} while (FALSE);

	//��鲢�ͷŻ�����
	if (fileDispInfo != NULL) {
		delete fileBasicInfo;
		fileBasicInfo = nullptr;
	}
	if (fileBasicInfo != nullptr) {
		delete fileBasicInfo;
		fileBasicInfo = nullptr;
	}
	return status;
}


//ǿɾ�ļ�(�豸���������е���)
NTSTATUS FileManager::ForceDeleteFile(PCWSTR fileName){
	PDEVICE_OBJECT DeviceObject = nullptr;
	PFILE_OBJECT FileObject = nullptr;
	NTSTATUS status = STATUS_NOT_SUPPORTED;
	IO_STATUS_BLOCK ioStatus;
	UNICODE_STRING unsFilePath;

	PAGED_CODE();
	

	//�Լ�����IRP_MJ_CREATE������ļ�
	status = IrpCreateFile(&FileObject,
		&DeviceObject,
		FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | DELETE | SYNCHRONIZE,
		fileName,
		&ioStatus,
		NULL,
		FILE_ATTRIBUTE_NORMAL,
		FILE_SHARE_VALID_FLAGS,
		FILE_OPEN,
		FILE_SYNCHRONOUS_IO_NONALERT,
		NULL,
		0);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	//ǿɾ�ļ�
	status = IrpDeleteFile(DeviceObject, FileObject);

	//�Լ���������IRP_MJ_CLEANUP/IRP_MJ_CLOSE�ر��ļ�
	status = IrpCleanupFile(DeviceObject,
		FileObject);
	ASSERT(NT_SUCCESS(status));

	if (NT_SUCCESS(status)) {
		status = IrpCloseFile(DeviceObject, FileObject);
		ASSERT(NT_SUCCESS(status));
	}

	return status;
}

NTSTATUS FileManager::SetInformationFile(PIO_STATUS_BLOCK ioStatus, PVOID FileInformation, ULONG Length, FILE_INFORMATION_CLASS FileInformationClass) {
	return ZwSetInformationFile(_handle, ioStatus, FileInformation, Length, FileInformationClass);
}