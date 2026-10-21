<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="html" indent="yes"/>

  <xsl:template match="/">
    <ul>
      <xsl:for-each select="rfc/middle/section">
        <xsl:variable name="page" select="'sections/' || @anchor || '.html'"/>
        <!-- One page for each top-level section... -->
        <xsl:result-document href="{$page}">
          <h1>{name}</h1>
          <p>{count(.//t[@anchor])} requirements, {count(section)} subsections</p>
        </xsl:result-document>
        <!-- ...and a link to it in the index. -->
        <li><a href="{$page}">{name}</a></li>
      </xsl:for-each>
    </ul>
  </xsl:template>
</xsl:stylesheet>
