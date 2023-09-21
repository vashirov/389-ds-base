import cockpit from "cockpit";
import React from "react";
import {
    Grid,
    GridItem,
    Pagination,
    PaginationVariant,
    SearchInput,
    Tooltip,
} from '@patternfly/react-core';
import {
    expandable,
    Table,
    TableHeader,
    TableBody,
    TableVariant,
    sortable,
    SortByDirection,
} from '@patternfly/react-table';
import PropTypes from "prop-types";

const _ = cockpit.gettext;

class KeyTable extends React.Component {
    constructor(props) {
        super(props);

        this.state = {
            page: 1,
            perPage: 10,
            value: '',
            sortBy: {},
            rows: [],
            hasRows: false,
            columns: [
                { title: _("Cipher"), transforms: [sortable] },
                { title: _("Key Identifier"), transforms: [sortable] },
                { title: _("State"), transforms: [sortable] },
            ],
        };

        this.handleSetPage = (_event, pageNumber) => {
            this.setState({
                page: pageNumber
            });
        };

        this.handlePerPageSelect = (_event, perPage) => {
            this.setState({
                perPage
            });
        };
    }

    componentDidMount() {
        let rows = [];
        let columns = this.state.columns;
        let hasRows = true;

        for (const ServerKey of this.props.ServerKeys) {
            rows.push(
                {
                    isOpen: false,
                    cells: [ServerKey.attrs.cipher, ServerKey.attrs.key_id, ServerKey.attrs.state],
                },
            );
        }

        if (rows.length === 0) {
            rows = [{ cells: [_("No Orphan keys")] }];
            columns = [{ title: _("Orphan keys") }];
            hasRows = false;
        }

        this.setState({
            rows,
            columns,
            hasRows
        });
    }

    actions() {
        return [
            {
                title: _("Delete Key"),
                onClick: (event, rowId, rowData, extra) => {
                    if (rowData.cells[1]) {
                        this.props.delKey(rowData.cells[1]);
                    }
                }
            }
        ];
    }

    render() {
        const { perPage, page, sortBy, rows, columns, hasRows } = this.state;

        return (
            <div className="ds-margin-top-lg">
                <Tooltip
                        content={
                            <div>
                                <p>
                                    {_("An orphan key is a private key in the NSS DB for which there is NO cert with the corresponding public key. An orphan key is created during CSR creation, when the certificate associated with a CSR has been imported into the NSS DB its orphan state will be removed.")}
                                    <br /><br />
                                    {_("Make sure an orphan key is not associated with a submitted CSR before you delete it.")}
                                </p>
                            </div>
                        }
                >
                    <a className="ds-font-size-sm">{_("What is an orphan key?")}</a>
                </Tooltip>
                <Table
                    className="ds-margin-top"
                    aria-label="orph key table"
                    cells={columns}
                    key={rows}
                    rows={rows}
                    variant={TableVariant.compact}
                    sortBy={sortBy}
                    onSort={this.handleSort}
                    actions={hasRows ? this.actions() : null}
                    dropdownPosition="right"
                    dropdownDirection="bottom"
                >
                    <TableHeader />
                    <TableBody />
                </Table>
                {hasRows &&
                    <Pagination
                        itemCount={this.state.rows.length}
                        widgetId="pagination-options-menu-bottom"
                        perPage={perPage}
                        page={page}
                        variant={PaginationVariant.bottom}
                        onSetPage={this.handleSetPage}
                        onPerPageSelect={this.handlePerPageSelect}
                    />}
            </div>
        );
    }
}

class CSRTable extends React.Component {
    constructor(props) {
        super(props);

        this.state = {
            page: 1,
            perPage: 10,
            value: '',
            sortBy: {},
            rows: [],
            columns: [
                { title: _("Name"), transforms: [sortable] },
                { title: _("Subject DN"), transforms: [sortable] },
                { title: _("Subject Alternative Names"), transforms: [sortable] },
                { title: _("Modification Date"), transforms: [sortable] },
            ],
        };

        this.handleSetPage = (_event, pageNumber) => {
            this.setState({
                page: pageNumber
            });
        };

        this.handlePerPageSelect = (_event, perPage) => {
            this.setState({
                perPage,
                page: 1
            });
        };

        this.handleSort = this.handleSort.bind(this);
        this.handleSearchChange = this.handleSearchChange.bind(this);
    }

    handleSort(_event, index, direction) {
        const sortedRows = this.state.rows.sort((a, b) => (a[index] < b[index] ? -1 : a[index] > b[index] ? 1 : 0));
        this.setState({
            sortBy: {
                index,
                direction
            },
            rows: direction === SortByDirection.asc ? sortedRows : sortedRows.reverse()
        });
    }

    componentDidMount() {
        let rows = [];
        let columns = this.state.columns;
        let hasRows = true;

        for (const ServerCSR of this.props.ServerCSRs) {
            rows.push(
                {
                    isOpen: false,
                    cells: [
                        ServerCSR.attrs.name, ServerCSR.attrs.subject,
                        ServerCSR.attrs.subject_alt_names.join(", "), ServerCSR.attrs.modified
                    ],
                },
            );
        }
        if (rows.length === 0) {
            rows = [{ cells: [_("No Certificate Signing Requests")] }];
            columns = [{ title: _("Certificate Signing Requests") }];
            hasRows = false;
        }
        this.setState({
            rows,
            columns,
            hasRows,
        });
    }

    handleSearchChange(event, value) {
        const rows = [];

        for (const cert of this.props.ServerCSRs) {
            const val = value.toLowerCase();

            // Check for matches of all the parts
            if (val !== "" && cert.attrs.name.toLowerCase().indexOf(val) === -1 &&
                cert.attrs.subject.toLowerCase().indexOf(val) === -1 &&
                cert.attrs.subject_alt_names.join().toLowerCase()
                        .indexOf(val) === -1 &&
                cert.attrs.modified.toLowerCase().indexOf(val) === -1) {
                // Not a match
                continue;
            }

            rows.push(
                {
                    isOpen: false,
                    cells: [
                        cert.attrs.name, cert.attrs.subject,
                        cert.attrs.subject_alt_names.join(", "),
                        cert.attrs.modified
                    ],

                },
            );
        }

        this.setState({
            rows,
            value,
            page: 1,
        });
    }

    actions() {
        return [
            {
                title: _("Delete CSR"),
                onClick: (event, rowId, rowData, extra) => {
                    if (rowData.cells.length > 1) {
                        this.props.delCSR(rowData.cells[0]);
                    }
                }
            },
            {
                title: _("View CSR"),
                onClick: (event, rowId, rowData, extra) => {
                    if (rowData.cells.length > 1) {
                        this.props.viewCSR(rowData.cells[0]);
                    }
                }
            }
        ];
    }

    render() {
        const { perPage, page, sortBy, rows, columns, hasRows } = this.state;

        return (
            <div className="ds-margin-top-lg">
                {hasRows &&
                    <SearchInput
                        placeholder={_("Search CSRs")}
                        value={this.state.value}
                        onChange={this.handleSearchChange}
                        onClear={(evt) => this.handleSearchChange(evt, '')}
                    />}
                <Table
                    className="ds-margin-top"
                    aria-label="csr table"
                    cells={columns}
                    key={rows}
                    rows={rows}
                    variant={TableVariant.compact}
                    sortBy={sortBy}
                    onSort={this.handleSort}
                    actions={hasRows ? this.actions() : null}
                    dropdownPosition="right"
                    dropdownDirection="bottom"
                >
                    <TableHeader />
                    <TableBody />
                </Table>
                {hasRows &&
                    <Pagination
                        itemCount={rows.length}
                        widgetId="pagination-options-menu-bottom"
                        perPage={perPage}
                        page={page}
                        variant={PaginationVariant.bottom}
                        onSetPage={this.handleSetPage}
                        onPerPageSelect={this.handlePerPageSelect}
                    />}
            </div>
        );
    }
}

class CertTable extends React.Component {
    constructor(props) {
        super(props);

        this.state = {
            page: 1,
            perPage: 10,
            value: '',
            sortBy: {},
            rows: [],
            dropdownIsOpen: false,
            hasRows: false,
            columns: [
                {
                    title: _("Nickname"),
                    transforms: [sortable],
                    cellFormatters: [expandable]
                },
                { title: _("Subject DN"), transforms: [sortable] },
                { title: _("Expiration Date"), transforms: [sortable] },
            ],
        };

        this.handleSetPage = (_event, pageNumber) => {
            this.setState({
                page: pageNumber
            });
        };

        this.handlePerPageSelect = (_event, perPage) => {
            this.setState({
                perPage,
                page: 1
            });
        };

        this.handleSort = this.handleSort.bind(this);
        this.handleCollapse = this.handleCollapse.bind(this);
        this.handleSearchChange = this.handleSearchChange.bind(this);
    }

    handleSort(_event, index, direction) {
        const sorted_rows = [];
        const rows = [];
        let count = 0;

        // Convert the rows pairings into a sortable array based on the column indexes
        for (let idx = 0; idx < this.state.rows.length; idx += 2) {
            sorted_rows.push({
                expandedRow: this.state.rows[idx + 1],
                1: this.state.rows[idx].cells[0],
                2: this.state.rows[idx].cells[1],
                3: this.state.rows[idx].cells[2],
                issuer: this.state.rows[idx].issuer,
                flags: this.state.rows[idx].flags
            });
        }

        // Sort the rows and build the new rows
        sorted_rows.sort((a, b) => (a[index] > b[index]) ? 1 : -1);
        if (direction !== SortByDirection.asc) {
            sorted_rows.reverse();
        }
        for (const srow of sorted_rows) {
            rows.push({
                isOpen: false,
                cells: [
                    srow[1], srow[2], srow[3]
                ],
                issuer: srow.issuer,
                flags: srow.flags,
            });
            srow.expandedRow.parent = count; // reset parent idx
            rows.push(srow.expandedRow);
            count += 2;
        }

        this.setState({
            sortBy: {
                index,
                direction
            },
            rows,
            page: 1,
        });
    }

    getExpandedRow(issuer, flags) {
        return (
            <Grid className="ds-left-indent-lg">
                <GridItem span={3}>{_("Issuer DN:")}</GridItem>
                <GridItem span={9}><b>{issuer}</b></GridItem>
                <GridItem span={3}>{_("Trust Flags:")}</GridItem>
                <GridItem span={9}><b>{flags}</b></GridItem>

            </Grid>
        );
    }

    componentDidMount() {
        let rows = [];
        let columns = this.state.columns;
        let count = 0;
        let hasRows = true;

        for (const cert of this.props.certs) {
            rows.push(
                {
                    isOpen: false,
                    cells: [cert.attrs.nickname, cert.attrs.subject, cert.attrs.expires],
                    issuer: cert.attrs.issuer,
                    flags: cert.attrs.flags,

                },
                {
                    parent: count,
                    fullWidth: true,
                    cells: [{ title: this.getExpandedRow(cert.attrs.issuer, cert.attrs.flags) }]
                },
            );
            count += 2;
        }
        if (rows.length === 0) {
            rows = [{ cells: [_("No Certificates")] }];
            columns = [{ title: _("Certificates") }];
            hasRows = false;
        }
        this.setState({
            rows,
            columns,
            hasRows,
        });
    }

    handleCollapse(event, rowKey, isOpen) {
        const { rows, perPage, page } = this.state;
        const index = (perPage * (page - 1) * 2) + rowKey; // Adjust for page set
        rows[index].isOpen = isOpen;
        this.setState({
            rows
        });
    }

    handleSearchChange(event, value) {
        const rows = [];
        let count = 0;

        for (const cert of this.props.certs) {
            const val = value.toLowerCase();

            // Check for matches of all the parts
            if (val !== "" && cert.attrs.nickname.toLowerCase().indexOf(val) === -1 &&
                cert.attrs.subject.toLowerCase().indexOf(val) === -1 &&
                cert.attrs.issuer.toLowerCase().indexOf(val) === -1 &&
                cert.attrs.expires.toLowerCase().indexOf(val) === -1) {
                // Not a match
                continue;
            }

            rows.push(
                {
                    isOpen: false,
                    cells: [cert.attrs.nickname, cert.attrs.subject, cert.attrs.expires],
                    issuer: cert.attrs.issuer,
                    flags: cert.attrs.flags,

                },
                {
                    parent: count,
                    fullWidth: true,
                    cells: [{ title: this.getExpandedRow(cert.attrs.issuer, cert.attrs.flags) }]
                },
            );
            count += 2;
        }

        this.setState({
            rows,
            value,
            page: 1,
        });
    }

    actions() {
        return [
            {
                title: _("Edit Trust Flags"),
                onClick: (event, rowId, rowData, extra) =>
                    this.props.editCert(rowData.cells[0], rowData.flags)
            },
            {
                title: _("Export Certificate"),
                onClick: (event, rowId, rowData, extra) =>
                    this.props.exportCert(rowData.cells[0])
            },
            {
                isSeparator: true
            },
            {
                title: _("Delete Certificate"),
                onClick: (event, rowId, rowData, extra) =>
                    this.props.delCert(rowData.cells[0])
            }
        ];
    }

    render() {
        const { perPage, page, sortBy, rows, columns, hasRows } = this.state;
        const origRows = [...rows];
        const startIdx = ((perPage * page) - perPage) * 2;
        const tableRows = origRows.splice(startIdx, perPage * 2);

        for (let idx = 1, count = 0; idx < tableRows.length; idx += 2, count += 2) {
            // Rewrite parent index to match new spliced array
            tableRows[idx].parent = count;
        }

        return (
            <div className="ds-margin-top-lg">
                {hasRows &&
                    <SearchInput
                        placeholder={_("Search Certificates")}
                        value={this.state.value}
                        onChange={this.handleSearchChange}
                        onClear={(evt) => this.handleSearchChange(evt, '')}
                    />}
                <Table
                    className="ds-margin-top"
                    aria-label="cert table"
                    cells={columns}
                    key={tableRows}
                    rows={tableRows}
                    variant={TableVariant.compact}
                    sortBy={sortBy}
                    onSort={this.handleSort}
                    onCollapse={this.handleCollapse}
                    actions={hasRows ? this.actions() : null}
                    dropdownPosition="right"
                    dropdownDirection="bottom"
                >
                    <TableHeader />
                    <TableBody />
                </Table>
                {hasRows &&
                    <Pagination
                        itemCount={this.state.rows.length / 2}
                        widgetId="pagination-options-menu-bottom"
                        perPage={perPage}
                        page={page}
                        variant={PaginationVariant.bottom}
                        onSetPage={this.handleSetPage}
                        onPerPageSelect={this.handlePerPageSelect}
                    />}
            </div>
        );
    }
}

// Future - https://github.com/389ds/389-ds-base/issues/3548
class CRLTable extends React.Component {
    constructor(props) {
        super(props);

        this.state = {
            page: 1,
            perPage: 10,
            value: '',
            sortBy: {},
            rows: [],
            dropdownIsOpen: false,
            hasRows: false,
            columns: [
                { title: _("Issued By"), transforms: [sortable] },
                { title: _("Effective Date"), transforms: [sortable] },
                { title: _("Next Update"), transforms: [sortable] },
                { title: _("Type"), transforms: [sortable] },
            ],
        };

        this.handleSetPage = (_event, pageNumber) => {
            this.setState({
                page: pageNumber
            });
        };

        this.handlePerPageSelect = (_event, perPage) => {
            this.setState({
                perPage
            });
        };

        this.handleSort = this.handleSort.bind(this);
        this.handleSearchChange = this.handleSearchChange.bind(this);
    }

    handleSort(_event, index, direction) {
        const sorted_rows = [];
        const rows = [];
        let count = 0;

        // Convert the rows pairings into a sortable array based on the column indexes
        for (let idx = 0; idx < this.state.rows.length; idx += 2) {
            sorted_rows.push({
                1: this.state.rows[idx].cells[0],
                2: this.state.rows[idx].cells[1],
                3: this.state.rows[idx].cells[2],
                issuer: this.state.rows[idx].issuer,
                flags: this.state.rows[idx].flags
            });
        }

        // Sort the rows and build the new rows
        sorted_rows.sort((a, b) => (a[index] > b[index]) ? 1 : -1);
        if (direction !== SortByDirection.asc) {
            sorted_rows.reverse();
        }
        for (const srow of sorted_rows) {
            rows.push({
                isOpen: false,
                cells: [
                    srow[1], srow[2], srow[3]
                ],
                issuer: srow.issuer,
                flags: srow.flags,
            });
            srow.expandedRow.parent = count; // reset parent idx
            rows.push(srow.expandedRow);
            count += 2;
        }

        this.setState({
            sortBy: {
                index,
                direction
            },
            rows,
            page: 1,
        });
    }

    handleSearchChange(event, value) {
        const rows = [];
        let count = 0;

        for (const cert of this.props.certs) {
            const val = value.toLowerCase();

            // Check for matches of all the parts
            if (val !== "" && cert.attrs.nickname.toLowerCase().indexOf(val) === -1 &&
                cert.attrs.subject.toLowerCase().indexOf(val) === -1 &&
                cert.attrs.issuer.toLowerCase().indexOf(val) === -1 &&
                cert.attrs.expires.toLowerCase().indexOf(val) === -1) {
                // Not a match
                continue;
            }

            rows.push(
                {
                    isOpen: false,
                    cells: [cert.attrs.nickname, cert.attrs.subject, cert.attrs.expires],
                    issuer: cert.attrs.issuer,
                    flags: cert.attrs.flags,

                },
                {
                    parent: count,
                    fullWidth: true,
                    cells: [{ title: this.getExpandedRow(cert.attrs.issuer, cert.attrs.flags) }]
                },
            );
            count += 2;
        }

        this.setState({
            rows,
            value,
            page: 1,
            hasRows: rows.length !== 0,
        });
    }

    actions() {
        return [
            {
                title: _("View CRL"),
                onClick: (event, rowId, rowData, extra) =>
                    this.props.editConfig(rowData.cells[0], rowData.cells[1], rowData.credsBindpw, rowData.pwInteractive)
            },
            {
                title: _("Delete CRL"),
                onClick: (event, rowId, rowData, extra) =>
                    this.props.deleteConfig(rowData.cells[0])
            }
        ];
    }

    render() {
        return (
            <div className="ds-margin-top">
                <SearchInput
                    placeholder={_("Search CRL's")}
                    value={this.state.value}
                    onChange={this.handleSearchChange}
                    onClear={(evt) => this.handleSearchChange(evt, '')}
                />
                <Table
                    variant={TableVariant.compact} aria-label="Cred Table"
                    sortBy={this.sortBy} onSort={this.handleSort} cells={this.state.columns}
                    rows={this.state.rows}
                    actions={this.state.hasRows ? this.actions() : null}
                    dropdownPosition="right"
                    dropdownDirection="bottom"
                >
                    <TableHeader />
                    <TableBody />
                </Table>
                {this.state.hasRows &&
                    <Pagination
                        itemCount={this.state.rows.length}
                        widgetId="pagination-options-menu-bottom"
                        perPage={this.state.perPage}
                        page={this.state.page}
                        variant={PaginationVariant.bottom}
                        onSetPage={this.handleSetPage}
                        onPerPageSelect={this.handlePerPageSelect}
                    />}
            </div>
        );
    }
}

// Props and defaults

CertTable.propTypes = {
    // serverId: PropTypes.string,
    certs: PropTypes.array,
    editCert: PropTypes.func,
    delCert: PropTypes.func,
};

CertTable.defaultProps = {
    // serverId: "",
    certs: [],
};

CSRTable.propTypes = {
    ServerCSRs: PropTypes.array,
    delCSR: PropTypes.func,
    viewCSR: PropTypes.func,
};

CSRTable.defaultProps = {
    ServerCSRs: [],
};

KeyTable.propTypes = {
    ServerKeys: PropTypes.array,
    delKey: PropTypes.func,
};

KeyTable.defaultProps = {
    ServerKeys: [],
};
export {
    CertTable,
    CRLTable,
    CSRTable,
    KeyTable,
};
